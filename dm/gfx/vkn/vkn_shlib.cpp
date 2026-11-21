// [DOS2] VK-NATIVE shader library (vkn_shlib.h: the contract, the inventory of the old chain and the switch table). The translator's
// shader loader for mode 1, taken out of xbg_game.cpp: the pack reader, the load-time SPIR-V rewrites with their conditions and
// switches, the per-draw variants ("#at" alpha test, "#vi" vertex retype), module creation. The rewrites themselves are the engine's
// pure translation units (xbg_spirv_*.cpp, xbg_vertex.c, xbg_spvopt.cpp, nr_ubodyn.c), linked as they are; what lived only inside
// xbg_game.cpp (the conditions, the switch reading, the device facts, the readers, the small rewrites and analyses) is here, in the
// same order. No translator state: nothing of xbg_game.cpp is called or initialised.
// Threads: one mutex (g_mu) for the shader map and every load (the translator's engine mutex did the same); vkCreateShaderModule
// additionally under the module lock while DM_GFX_PIPE_SERIAL is on. Callers: vkn_shader.c's worker thread (8 MB stack: a load runs
// the rewrites, deep on the stack) for shaders and variants.
#ifdef DM_GAME_DOS2
#include "vkn_shlib.h"
#include "vkn_shlib_named.h"   // [stage 5 A1] engine shaders by name (DM_VK_NATIVE_SH): the hook at the head of read_bundle
#include "vkn_post.h"         // [stage 5.2 B3] the fused post bundles (DM_VK_NATIVE_POST): the second hook of read_bundle
#include "vkn.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../vulkan/engine/xbg_vendor_policy.h"
#include "../vulkan/engine/xbg_adreno_policy.h"
#include "../vulkan/engine/xbg_pipeguard.h"
#include "../vulkan/engine/xbg_selftest.h"
#include "../vulkan/engine/xbg_spvopt.h"
#include "../vulkan/engine/xbg_spirv_quad.h"
#include "../vulkan/engine/xbg_spirv_lone.h"
#include "../vulkan/engine/xbg_spirv_gscull.h"
#include "../vulkan/engine/xbg_spirv_warp.h"
#include "../vulkan/engine/xbg_spirv_ldc.h"
#include "../vulkan/engine/xbg_spirv_temps.h"
#include "../vulkan/engine/xbg_spirv_peep.h"
#include "../vulkan/engine/xbg_spirv_ubovec.h"
#include "../vulkan/engine/xbg_spirv_idvs.h"
#include "../vulkan/engine/xbg_spirv_gather.h"
#include "../vulkan/engine/xbg_spirv_alpha.h"
#include "../vulkan/engine/xbg_vertex.h"
#include "../vulkan/engine/xbg_vertex_retype.h"
#include "../nr/nr_ubodyn.h"
#include "../vulkan/engine/xbg_spirv_pos.h"      // [stage 5.2 B1] position programs (vkn_shlib_vs_pos)
#include "../vulkan/engine/xbg_spirv_raster.h"   // [stage 5.2 B1] fragment output locations (vkn_shlib_fs_outputs)
#include "vkn_policy.h"                          // [stage 5.2 B1] DM_VK_NATIVE_LOAD by family
#include "vkn_occ.h"                             // [stage 5 technique A] read masks of constant blocks (vkn_shlib_read_mask)

extern "C" const char *dm_vk_driver_status(void) __attribute__((weak));   // vk_driver.c: the running driver (pipeguard list suffix)
extern "C" int xbg_sha256_hex16(const void *data, size_t n, char out_hex17[17]);   // xbg_shader_key.c (pure)

#define SL_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
namespace {
// ---------------------------------------------------------------- the configuration (vkn_shlib_init)
struct Cfg {
    bool ready = false;
    vkn_shlib_config c{};
    std::string dir, base;                       // shader directory and its parent
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceFeatures core{};             // cfg.enabled_core after DM_GFX_SIM_GPU
    bool ffl2 = false;                           // format_feature_flags2
    uint32_t sim = 0; XbgGpuFamily family = XBG_GPU_OTHER, policy = XBG_GPU_OTHER;
    // switches
    bool quad = false, warp32 = false, ldc_fold = false, forward_temps = false, nan_minmax = false, nan_sat = false, ldc_canon = false,
         f2i_notrunc = false, texcoord_unguard = false, alpha = true, serial = false, retype_keep = false;
    int ubovec = 0, idvs = 0, early_z = 1, invariant = 0, gscull = 0x61, gather_snap = 1, spv_opt = 0, warp_mode = 1;
    int ubd_mode = 0; bool ubd_ro = false, ubd_scan = false;   // NR-08b (nrubd::init)
    bool pos_on = false;   // [stage 5.2 B1] DM_VK_NATIVE_LOAD != 0 for the policy family: position programs / output masks built at load
    bool noout = false;    // [stage 5.3 V2] DM_GFX_NOOUT_MASK for the policy family (vkn_drv.c): the "no colour output" fact built at load
};
Cfg g;
std::mutex g_mu;          // the shader map and every load
std::mutex g_create_mu;   // the module lock (DM_GFX_PIPE_SERIAL)
std::atomic<uint64_t> st_loads{0}, st_missing{0}, st_unusable{0}, st_pack{0}, st_file{0}, st_native{0}, st_alpha{0}, st_alpha_ref{0},
    st_retype{0}, st_retype_ref{0}, st_mod_fail{0}, st_gaps_hard{0}, st_ns{0};
std::atomic<int> g_reds{0};

void L(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void L(const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    vkn_logf("shlib: %s", b);
}
#define RED(...) do { if (g_reds.fetch_add(1) < 64) vkn_logf(SL_RED "shlib: " __VA_ARGS__); } while (0)
uint64_t now_ns() { return vkn_now_ns(); }

// ---------------------------------------------------------------- the pack reader (xbg_game.cpp xpack 4821-4915, native 4853-4901)
struct Ent { char key[32]; uint64_t off; uint32_t size, pad; uint64_t hash; };
static_assert(sizeof(Ent) == 56, "xgspack index entry");
uint32_t crc32_of(const uint8_t *b, size_t n) {   // zlib CRC-32 (xgs_pack.py), as xpack::crc
    static uint32_t t[256]; static std::once_flag once;
    std::call_once(once, [] { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; } });
    uint32_t c = 0xffffffffu; for (size_t i = 0; i < n; i++) c = t[(c ^ b[i]) & 0xff] ^ (c >> 8); return c ^ 0xffffffffu;
}
// An xgspack file: magic, version 1, count, sorted index of 56-byte entries inside the file. false = absent / damaged (closed).
bool pack_open(const std::string &path, int &fd, std::vector<Ent> &idx, bool &damaged) {
    damaged = false; const int f = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); if (f < 0) return false;
    struct stat st{}; uint32_t h[4] = {};
    bool ok = !fstat(f, &st) && pread(f, h, 16, 0) == 16 && h[0] == 0x50534758u && h[1] == 1u && h[2] < (1u << 22);
    std::vector<Ent> v; if (ok) { v.resize(h[2]); ok = pread(f, v.data(), v.size() * sizeof(Ent), 16) == ssize_t(v.size() * sizeof(Ent)); }
    for (size_t i = 0; ok && i < v.size(); i++) ok = v[i].key[31] == 0 && v[i].off >= 16 + v.size() * sizeof(Ent) && v[i].off + v[i].size <= uint64_t(st.st_size) && (!i || strcmp(v[i - 1].key, v[i].key) < 0);
    if (!ok) { ::close(f); damaged = true; return false; }
    fd = f; idx.swap(v); return true;
}
// 1 = found and its hash agrees, 0 = not listed, -1 = listed but unreadable / hash differs
int pack_get(int fd, const std::vector<Ent> &idx, const std::string &key, std::vector<uint8_t> &out) {
    auto it = std::lower_bound(idx.begin(), idx.end(), key, [](const Ent &a, const std::string &k) { return strcmp(a.key, k.c_str()) < 0; });
    if (it == idx.end() || key != it->key) return 0;
    out.resize(it->size); size_t got = 0;
    while (got < out.size()) { ssize_t r = pread(fd, out.data() + got, out.size() - got, off_t(it->off + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += size_t(r); }
    return got == out.size() && crc32_of(out.data(), out.size()) == it->hash ? 1 : -1;
}
namespace xp {   // shaders.xgspack (DM_GFX_SHADER_PACK)
std::mutex mu; int fd = -1, mode = -1; std::vector<Ent> idx; std::atomic<uint64_t> hits{0}, misses{0}, bad{0}, checks{0}, diffs{0};
void open_once() {
    std::lock_guard<std::mutex> l(mu); if (mode >= 0) return;
    const char *e = getenv("DM_GFX_SHADER_PACK"); mode = e && *e ? atoi(e) : 1; if (mode < 0 || mode > 2) mode = 1;
    if (!mode || g.dir.empty()) return;
    const std::string path = g.dir + "/shaders.xgspack"; bool damaged = false;
    if (!pack_open(path, fd, idx, damaged)) { L(damaged ? "shader pack %s is damaged or of another version: ignored (bundles read one file each)" : "shader pack %s: none (bundles read one file each)", path.c_str()); return; }
    L("shader pack %s: %zu bundles (DM_GFX_SHADER_PACK=%d%s)", path.c_str(), idx.size(), mode, mode == 2 ? " check: files decide" : "");
}
bool get(const std::string &key, std::vector<uint8_t> &out) {
    open_once(); if (fd < 0) return false;
    const int r = pack_get(fd, idx, key, out);
    if (!r) { ++misses; return false; }
    if (r < 0) { if (bad++ < 8) L("shader pack: bundle %s unreadable or its hash differs: read from its file", key.c_str()); return false; }
    ++hits; return true;
}
void check(const std::string &key, const std::vector<uint8_t> &file) {
    std::vector<uint8_t> p; if (!get(key, p)) return; ++checks;
    if (p != file && diffs++ < 8) L("shader pack check: %s differs from its file (pack %zu bytes, file %zu): rebuild the pack", key.c_str(), p.size(), file.size());
}
}
namespace nat {   // native.xgspack / native/<key>.xgs (DM_GFX_NATIVE_SHADERS)
int mode = -1, fd = -1; std::vector<Ent> idx; std::once_flag once; std::atomic<uint64_t> used{0}, stale{0}, checked{0}, differ{0};
int on() { if (mode < 0) { const char *e = getenv("DM_GFX_NATIVE_SHADERS"); int m = e && *e ? atoi(e) : 0; mode = m < 0 || m > 2 ? 0 : m; } return mode; }
void open_pack() {
    std::call_once(once, [] {
        if (g.dir.empty()) return;
        const std::string path = g.dir + "/native.xgspack"; bool damaged = false;
        if (!pack_open(path, fd, idx, damaged)) {
            if (damaged) L("native shaders: %s is damaged or of another version: ignored", path.c_str());
            else L("native shaders DM_GFX_NATIVE_SHADERS=%d: no %s (files native/<key>.xgs only)", mode, path.c_str());
            return;
        }
        L("native shaders DM_GFX_NATIVE_SHADERS=%d: %s, %zu bundles", mode, path.c_str(), idx.size());
    });
}
bool get(const std::string &key, std::vector<uint8_t> &out) {
    open_pack();
    // listed and sound: taken; not listed in a non-empty pack: none (a pack lists every native bundle); listed but unreadable: its file
    if (fd >= 0) { const int r = pack_get(fd, idx, key, out); if (r > 0) return true; if (!r && !idx.empty()) return false; }
    FILE *f = fopen((g.dir + "/native/" + key + ".xgs").c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); if (n < 32 || n > 16 * 1024 * 1024) { fclose(f); return false; }
    out.resize(size_t(n)); bool ok = fread(out.data(), 1, out.size(), f) == out.size(); fclose(f); return ok;
}
size_t head(const std::vector<uint8_t> &b) {   // bytes before the SPIR-V; 0 = not a well-formed bundle
    if (b.size() < 32) return 0;
    uint32_t h[8]; memcpy(h, b.data(), 32);
    if (h[0] != 0x31534758u || h[1] != 1 || h[3] > 256 || h[4] > 16 || h[5] > 65536 || h[6] % 4) return 0;
    size_t n = 32 + size_t(h[3]) * 32 + size_t(h[4]) * 4 + h[5]; return n + h[6] == b.size() ? n : 0;
}
void io_masks(const std::vector<uint8_t> &b, size_t off, uint32_t &in, uint32_t &out) {
    in = out = 0; const size_t n = (b.size() - off) / 4; std::vector<uint32_t> w(n); memcpy(w.data(), b.data() + off, n * 4);
    std::unordered_map<uint32_t, uint32_t> loc, sc; std::vector<uint32_t> iface;
    for (size_t i = 5; i < n;) {
        uint32_t c = w[i] >> 16, op = w[i] & 0xffff; if (!c || i + c > n) break;
        if (op == 15 && c > 3) { size_t k = i + 3; while (k < i + c && (w[k] & 0xff000000u) && (w[k] & 0xff0000u) && (w[k] & 0xff00u) && (w[k] & 0xffu)) k++; for (k++; k < i + c; k++) iface.push_back(w[k]); }
        else if (op == 71 && c >= 4 && w[i + 2] == 30) loc[w[i + 1]] = w[i + 3];
        else if (op == 59 && c >= 4) sc[w[i + 2]] = w[i + 3];
        i += c;
    }
    for (uint32_t v : iface) { auto l = loc.find(v), s = sc.find(v); if (l == loc.end() || s == sc.end() || l->second >= 32) continue; if (s->second == 1) in |= 1u << l->second; else if (s->second == 3) out |= 1u << l->second; }
}
void apply(const std::string &key, std::vector<uint8_t> &bytes) {
    std::vector<uint8_t> nb; if (!get(key, nb)) return;
    // the head minus header word 6 (the SPIR-V byte count): a native bundle's SPIR-V has its own length
    const size_t th = head(bytes), nh = head(nb);
    const bool same = th && nh == th && !memcmp(bytes.data(), nb.data(), 24) && !memcmp(bytes.data() + 28, nb.data() + 28, th - 28);
    if (mode == 1) {
        if (same) { bytes.swap(nb); const uint64_t u = ++used; ++st_native; if (u <= 4 || !(u & (u - 1))) L("native shader %s used (%llu so far)", key.c_str(), (unsigned long long)u); }
        else if (stale++ < 16) L("native shader %s: head differs from the translated bundle (rebuild with tools/dos2_native_shaders.py): translated used", key.c_str());
        return;
    }
    ++checked; uint32_t ti = 0, to = 0, ni = 0, no = 0; if (th) io_masks(bytes, th, ti, to); if (nh) io_masks(nb, nh, ni, no);
    if (!same || ti != ni || to != no) { if (differ++ < 64) L("native shader %s mismatch: head %s, inputs %#x/%#x outputs %#x/%#x (translated/native)", key.c_str(), same ? "same" : "differs", ti, ni, to, no); }
}
}
// One bundle: pack first (mode 1), else its own file; then the native swap. false = neither has it; a bad length leaves out empty.
bool read_bundle(const std::string &key, std::vector<uint8_t> &out, bool &from_pack) {
    { const int nr = vkn_shlib_named_read(key, out, from_pack, read_bundle, g.dir, g.c.physical, g.ffl2, int(g.policy)); if (nr >= 0) return nr != 0; }   // [stage 5 A1] DM_VK_NATIVE_SH (vkn_shlib_named.h): NR-04 keys read here (the named bundle in place of the translated one)
    if (vkn_post_read) { const int pr = vkn_post_read(key, out, from_pack, read_bundle, g.dir); if (pr >= 0) return pr != 0; }   // [stage 5.2 B3] "F1." / "F1A." / "F2." keys (vkn_post.h)
    xp::open_once(); from_pack = false; bool have = false;
    if (xp::mode == 1 && xp::get(key, out)) { from_pack = true; have = true; }
    else {
        FILE *f = fopen((g.dir + "/" + key + ".xgs").c_str(), "rb"); if (!f) return false;
        fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
        if (n < 32 || n > 16 * 1024 * 1024) { fclose(f); out.clear(); return true; }
        out.resize(size_t(n)); bool ok = fread(out.data(), 1, out.size(), f) == out.size(); fclose(f); if (!ok) out.clear();
        if (ok && xp::mode == 2) xp::check(key, out);
        have = true;
    }
    if (have && !out.empty() && nat::on()) nat::apply(key, out);
    return have;
}
// spv-override/<key>.spv (a diagnostic: this SPIR-V replaces the bundle's); the directory listed once (DM_GFX_SPV_OVERRIDE_PROBE=1 probes)
std::mutex spv_mu; int spv_listed = -1; std::unordered_set<std::string> spv_names;
bool spv_may_exist(const std::string &key) {
    std::lock_guard<std::mutex> l(spv_mu);
    if (spv_listed < 0) {
        const char *e = getenv("DM_GFX_SPV_OVERRIDE_PROBE"); spv_listed = e && *e == '1' ? 0 : 1;
        if (spv_listed) {
            if (DIR *d = opendir((g.base + "/spv-override").c_str())) { while (dirent *de = readdir(d)) { std::string n = de->d_name; if (n.size() > 4 && !n.compare(n.size() - 4, 4, ".spv")) spv_names.insert(n.substr(0, n.size() - 4)); } closedir(d); }
            L("spv-override listed once: %zu module(s)", spv_names.size());
        }
    }
    return !spv_listed || spv_names.count(key);
}
bool spv_override(const std::string &key, std::vector<uint32_t> &words) {
    if (!spv_may_exist(key)) return false;
    FILE *f = fopen((g.base + "/spv-override/" + key + ".spv").c_str(), "rb"); if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f); if (n < 20 || n % 4) { fclose(f); return false; }
    std::vector<uint32_t> w(size_t(n) / 4); bool ok = fread(w.data(), 1, size_t(n), f) == size_t(n); fclose(f);
    if (!ok || w[0] != 0x07230203u) return false;
    words.swap(w); L("spv override %s words=%zu", key.c_str(), words.size()); return true;
}

// ---------------------------------------------------------------- the lone-lane list (pipeguard v2 read side, xbg_game.cpp 5632)
namespace lone {
std::mutex mu; bool loaded = false; std::unordered_map<std::string, std::string> keys;   // key -> spv it was listed for
int pg_mode() { static const int m = [] { const char *e = getenv("DM_PIPEGUARD"); return e && *e == '1' ? 1 : 2; }(); return m; }
std::string hex16(const void *d, size_t n) { char o[17] = {0}; xbg_sha256_hex16(d, n, o); return o; }
std::string header() {
    std::string tr = "-";
    if (FILE *f = fopen((g.dir + "/_chain.json").c_str(), "rb")) { std::string c; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) c.append(buf, n); fclose(f); if (!c.empty()) tr = hex16(c.data(), c.size()); }
    const char *s = dm_vk_driver_status ? dm_vk_driver_status() : nullptr;
    const std::string drv = xbg_pg::guard_driver(getenv("DM_PIPEGUARD_DRIVER"), s ? s : "");
    return xbg_pg::header(drv.c_str(), g.props.driverVersion, g.props.pipelineCacheUUID, VK_UUID_SIZE, tr.c_str());
}
std::string suffix() { const char *s = dm_vk_driver_status ? dm_vk_driver_status() : nullptr; return xbg_pg::suffix(xbg_pg::guard_driver(getenv("DM_PIPEGUARD_DRIVER"), s ? s : "")); }
// Listed for this driver, and (v2) its SPIR-V as read is the one the entry names.
bool forced(const std::string &key, const std::vector<uint32_t> &read_words) {
    std::string spv;
    {
        std::lock_guard<std::mutex> l(mu);
        if (!loaded) { loaded = true; xbg_pg::read_list(g.base + "/gfx-lone-lane" + suffix(), header(), pg_mode(), keys, [](const std::string &s) { L("%s", s.c_str()); }); }
        auto f = keys.find(key); if (f == keys.end()) return false; spv = f->second;
    }
    return xbg_pg::applies(spv, pg_mode() < 2 ? std::string() : hex16(read_words.data(), read_words.size() * 4), pg_mode());
}
}

// ---------------------------------------------------------------- capabilities (validate_capabilities / module_gap 4485-4497)
// "" = usable; soft = the capability gap the translator degrades (geometry, formatless storage); hard = what it would abort on.
void caps(const std::vector<uint32_t> &w, uint32_t stage, std::string &soft, std::string &hard) {
    soft.clear(); hard.clear();
    char b[160];
    if (!(w.size() >= 5 && w[0] == 0x07230203)) { hard = "Invalid packaged SPIR-V"; return; }
    bool group_operations = false; VkSubgroupFeatureFlags groups = 0;
    for (size_t i = 5; i < w.size();) {
        uint32_t count = w[i] >> 16, op = w[i] & 65535;
        if (!(count && i + count <= w.size())) { hard = "Invalid SPIR-V instruction bounds"; return; }
        if (op >= 333 && op <= 366) group_operations = true;
        if (op == 17) {
            uint32_t cap = w[i + 1]; bool ok = true; const VkPhysicalDeviceFeatures &f = g.core;
            switch (cap) {
            case 1: case 46: case 47: case 50: break;
            case 40: ok = stage == 4; break;
            case 2: ok = f.geometryShader; if (!ok) { soft = "geometryShader"; ok = true; } break;
            case 3: ok = f.tessellationShader; break;
            case 10: ok = f.shaderFloat64; break;
            case 11: ok = f.shaderInt64; break;
            case 22: ok = f.shaderInt16; break;
            case 25: ok = f.shaderImageGatherExtended; break;
            case 32: ok = f.shaderClipDistance; break;
            case 33: ok = f.shaderCullDistance; break;
            case 35: ok = f.sampleRateShading; break;
            case 49: ok = f.shaderStorageImageExtendedFormats; break;
            case 55: ok = f.shaderStorageImageReadWithoutFormat || g.ffl2; if (!ok) { soft = "shaderStorageImageReadWithoutFormat"; ok = true; } break;
            case 56: ok = f.shaderStorageImageWriteWithoutFormat || g.ffl2; if (!ok) { soft = "shaderStorageImageWriteWithoutFormat"; ok = true; } break;
            case 61: groups |= VK_SUBGROUP_FEATURE_BASIC_BIT; break;
            case 62: groups |= VK_SUBGROUP_FEATURE_VOTE_BIT; break;
            case 63: groups |= VK_SUBGROUP_FEATURE_ARITHMETIC_BIT; break;
            case 64: groups |= VK_SUBGROUP_FEATURE_BALLOT_BIT; break;
            case 65: groups |= VK_SUBGROUP_FEATURE_SHUFFLE_BIT; break;
            case 66: groups |= VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT; break;
            case 4427: ok = g.c.enabled_shader_draw_parameters; break;
            default: snprintf(b, sizeof b, "Shader stage %u has unhandled capability %u", stage, cap); hard = b; return;
            }
            if (!ok) { snprintf(b, sizeof b, "Shader stage %u requires disabled capability %u", stage, cap); hard = b; return; }
        }
        if (op == 10) { snprintf(b, sizeof b, "Shader stage %u declares extension requiring explicit backend support", stage); hard = b; return; }
        i += count;
    }
    if ((g.subgroup.supportedOperations & groups) != groups) { hard = "Required subgroup operations unavailable"; return; }
    if (group_operations && !(g.subgroup.supportedStages & (1u << stage))) { hard = "Actual subgroup operation unsupported in shader stage"; return; }
}

// ---------------------------------------------------------------- the load-time rewrites (xbg_game.cpp 4505-4815), in chain order
void sl_gscull(std::vector<uint32_t> &w, uint32_t stage, const char *key) {   // gscull_rewrite 4599
    const int mask = g.gscull;
    if ((mask & 0x800) && stage == 0) { size_t k = xbg_spirv_index_carrier(w, 0); if (k) L("index carrier shader=%s stage=0 conversions=%zu", key, k); }
    if (stage != 3 || !xbg_spirv_gscull_shape(w.data(), w.size())) return;
    if (mask & 0x800) xbg_spirv_index_carrier(w, 3);
    if (mask & 0x1000) xbg_spirv_cas_int_temps(w);
    if (!(mask & (30 | 0x200))) return;
    uint32_t applied = 0; if (mask & 30) xbg_spirv_gscull_relax(w, uint32_t(mask), &applied);
    if (mask & 0x200) xbg_spirv_gscull_identity(w);
}
void sl_lone(std::vector<uint32_t> &words, uint32_t stage, const char *key, const std::vector<uint32_t> &read_words) {   // lone_rewrite 4658
    if (words.size() < 5) return;
    bool ops = false; VkSubgroupFeatureFlags groups = 0;
    for (size_t i = 5; i < words.size();) {
        uint32_t count = words[i] >> 16, op = words[i] & 65535; if (!count || i + count > words.size()) return;
        if (op >= 333 && op <= 366) ops = true;
        if (op == 17 && count >= 2 && words[i + 1] >= 61 && words[i + 1] <= 66) groups |= VkSubgroupFeatureFlags(1u << (words[i + 1] - 61));
        if (op == 54) break;
        i += count;
    }
    if (!ops) for (size_t i = 5; i < words.size();) { uint32_t count = words[i] >> 16, op = words[i] & 65535; if (!count) break; if (op >= 333 && op <= 366) { ops = true; break; } i += count; }
    if (!ops && !groups) return;
    const bool gs_warp = stage == 3 && (g.gscull & 1);
    const bool forced = lone::forced(key, read_words);
    // DM_GFX_WARP_MODE (the translator's DM_GAME_XC2 || DM_GAME_D3 branch, which the DOS2 build takes: DOS2 = the TL2 build with D3): the
    // translated Maxwell code assumes a 32-lane warp; any other subgroup size gives every stage with subgroup operations the lone-lane
    // form, except compute while warp32 is on (32-lane warps emulated there)
    const bool warp_lone = g.warp_mode == 1 && !(stage == 5 && g.warp32 && g.subgroup.subgroupSize > 32);
    if (!forced && !gs_warp && !warp_lone && (g.subgroup.supportedStages & (1u << stage)) && (g.subgroup.supportedOperations & groups) == groups) return;
    std::vector<uint32_t> out; size_t k = xbg_spirv_lone_lane(words.data(), words.size(), out);
    if (!k) { L("lone-lane shader=%s stage=%u: subgroup use not rewritable (stages %#x ops %#x)", key, stage, g.subgroup.supportedStages, g.subgroup.supportedOperations); return; }
    words.swap(out);
    static std::atomic<unsigned> told{0};
    if (told++ < 16) L("lone-lane shader=%s stage=%u rewrites=%zu%s (device subgroup stages %#x ops %#x)", key, stage, k, forced ? " forced (compiler crash)" : gs_warp ? " forced (geometry warp idiom)" : "", g.subgroup.supportedStages, g.subgroup.supportedOperations);
}
void sl_early(std::vector<uint32_t> &w, uint32_t stage) {   // early_tests_rewrite 4562
    if (!g.early_z || stage != 4 || w.size() < 5) return;
    bool effects = false, blocked = false, has_mode = false; uint32_t entry = 0; size_t insert = 0;
    for (size_t i = 5; i < w.size();) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; if (!n || i + n > w.size()) return;
        if (op == 15 && n >= 3 && !entry) { entry = w[i + 2]; insert = i + n; }
        else if (op == 16 && n >= 3) { insert = i + n; if (w[i + 2] == 9) has_mode = true; if (w[i + 2] == 12 || w[i + 2] == 14 || w[i + 2] == 15) blocked = true; }
        else if (op == 71 && n >= 4 && w[i + 2] == 11 && w[i + 3] == 22) blocked = true;
        else if (op == 252 || op == 4416 || op == 5380) blocked = true;
        else if (op == 99 || (op >= 227 && op <= 242)) effects = true;
        i += n;
    }
    if (!entry || !insert || has_mode || blocked || !effects) return;
    const uint32_t ins[3] = {(3u << 16) | 16u, entry, 9u}; w.insert(w.begin() + ptrdiff_t(insert), ins, ins + 3);
}
void sl_invariant(std::vector<uint32_t> &w, uint32_t stage) {   // invariant_rewrite 4539
    if (!g.invariant || stage > 3 || w.size() < 5) return;
    std::vector<uint32_t> add; size_t insert = 0;
    for (size_t i = 5; i < w.size();) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; if (!n || i + n > w.size()) return;
        if (op == 71 || op == 72) {
            insert = i + n;
            if (op == 71 && n >= 4 && w[i + 2] == 11 && w[i + 3] == 0) { add.push_back((3u << 16) | 71u); add.push_back(w[i + 1]); add.push_back(18u); }
            if (op == 72 && n >= 5 && w[i + 3] == 11 && w[i + 4] == 0) { add.push_back((4u << 16) | 72u); add.push_back(w[i + 1]); add.push_back(w[i + 2]); add.push_back(18u); }
        } else if (op == 127 || op == 129 || op == 131 || op == 133 || op == 136 || op == 141 || op == 142 || op == 143 || op == 144 || op == 145 || op == 146 || op == 148) {
            if (n >= 3) { add.push_back((3u << 16) | 71u); add.push_back(w[i + 2]); add.push_back(42u); }
        } else if (op == 12 && n >= 5 && w[i + 4] == 50) { add.push_back((3u << 16) | 71u); add.push_back(w[i + 2]); add.push_back(42u); }
        i += n;
    }
    if (!insert || add.empty()) return;
    w.insert(w.begin() + ptrdiff_t(insert), add.begin(), add.end());
}
void sl_gather_ext(std::vector<uint32_t> &w, const char *key) {   // gather_ext_rewrite 4624
    if (g.core.shaderImageGatherExtended || w.size() < 5) return;
    std::vector<uint32_t> out(w.begin(), w.begin() + 5); size_t dropped = 0; bool cap = false;
    for (size_t i = 5; i < w.size();) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; if (!n || i + n > w.size()) return;
        if (op == 17 && n == 2 && w[i + 1] == 25) { cap = true; i += n; continue; }
        size_t mi = 0; if (op == 87 || op == 88 || op == 91 || op == 92 || op == 95 || op == 98) mi = 5; else if (op == 89 || op == 90 || op == 93 || op == 94 || op == 96 || op == 97) mi = 6;
        if (mi && n > mi && (w[i + mi] & 0x30)) {
            const uint32_t mask = w[i + mi]; std::vector<uint32_t> ins(w.begin() + long(i), w.begin() + long(i + mi)); uint32_t nm = mask & ~0x30u; ins.push_back(nm);
            size_t at = i + mi + 1; const uint32_t order[8] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80};
            for (uint32_t bit : order) { if (!(mask & bit)) continue; size_t k = bit == 0x4 ? 2 : 1; if (at + k > i + n) return; if (!(bit & 0x30)) ins.insert(ins.end(), w.begin() + long(at), w.begin() + long(at + k)); at += k; }
            ins[0] = (uint32_t(ins.size()) << 16) | op; out.insert(out.end(), ins.begin(), ins.end()); ++dropped; i += n; continue;
        }
        out.insert(out.end(), w.begin() + long(i), w.begin() + long(i + n)); i += n;
    }
    if (!cap && !dropped) return;
    w.swap(out);
    static std::atomic<unsigned> logged{0}; if ((dropped || logged.load() < 4) && logged++ < 64) L("gather-extended unavailable: shader=%s capability removed, %zu offset operand(s) dropped", key, dropped);
}
void sl_gather_snap(std::vector<uint32_t> &w, uint32_t stage) {   // gather_snap_rewrite 4652
    if (stage != 4 || !g.gather_snap) return;
    std::vector<uint32_t> out; xbg_spirv_gather_info info;
    if (xbg_spirv_gather_snap(w.data(), w.size(), g.gather_snap >= 2, out, &info)) return;
    w.swap(out);
}
void sl_quad(std::vector<uint32_t> &words, uint32_t stage) {   // quad_rewrite 4505
    if (!g.quad || stage != 4) return;
    std::vector<uint32_t> out; xbg_spirv_quad_stats counts;
    size_t k = xbg_spirv_quad_derivatives(words.data(), words.size(), out, &counts);
    if (k) words.swap(out);
}
void sl_warp(std::vector<uint32_t> &words, uint32_t stage, const char *key) {   // warp_rewrite 4696
    if (!g.warp32 || stage != 5 || g.subgroup.subgroupSize <= 32) return;
    std::vector<uint32_t> out; size_t k = xbg_spirv_warp32(words.data(), words.size(), g.subgroup.subgroupSize, out);
    if (!k) return;
    std::string soft, hard; caps(out, stage, soft, hard);   // a vote-only module can acquire Ballot capability while rewritten
    if (!hard.empty() || !soft.empty()) { RED("warp32 shader=%s: the rewritten module fails the capability check (%s; the translator aborted here): kept as it was", key, hard.empty() ? soft.c_str() : hard.c_str()); return; }
    words.swap(out);
}
size_t nan_minmax_swap(std::vector<uint32_t> &w, bool sat, size_t *kept_out) {   // nan_minmax_swap 4786
    uint32_t glsl = 0; size_t k = 0, kept = 0; std::unordered_map<uint32_t, uint32_t> fconst;
    for (size_t i = 5; i < w.size();) {
        uint32_t op = w[i] & 0xFFFFu, n = w[i] >> 16; if (!n || i + n > w.size()) break;
        if (sat && op == 43 && n == 4) fconst[w[i + 2]] = w[i + 3];
        if (op == 11 && n >= 5 && !memcmp(&w[i + 2], "GLSL.std.450", 13)) glsl = w[i + 1];
        else if (op == 12 && n >= 5 && glsl && w[i + 3] == glsl) {
            uint32_t &e = w[i + 4];
            if (e == 43 && sat && n >= 8) { auto lo = fconst.find(w[i + 6]), hi = fconst.find(w[i + 7]); if (lo != fconst.end() && hi != fconst.end() && lo->second == 0u && hi->second == 0x3f800000u) { ++kept; i += n; continue; } }
            if (e == 37) { e = 79; ++k; } else if (e == 40) { e = 80; ++k; } else if (e == 43) { e = 81; ++k; }
        }
        i += n;
    }
    if (kept_out) *kept_out = kept;
    return k;
}
size_t selftest_nan_swap(std::vector<uint32_t> &w, bool sat, size_t *kept) { return nan_minmax_swap(w, sat, kept); }
void sl_ubovec(std::vector<uint32_t> &words, const char *key) {   // ubovec_rewrite 4755
    if (!g.ubovec) return;
    std::vector<uint32_t> out; XbgUbovecStats st; const bool poison = g.ubovec == 3;
    const size_t k = xbg_spirv_ubovec(words.data(), words.size(), out, &st, poison);
    if (!k) return;
    const char *bad = xbg_spirv_ubovec_check(words.data(), words.size(), out.data(), out.size(), poison);
    if (*bad) { RED("DM_NR_UBOVEC=%d check: shader %s: %s: the module is used as it was", g.ubovec, key, bad); return; }
    if (g.ubovec != 2) words.swap(out);
}
void sl_tidy(std::vector<uint32_t> &words, const char *key) {   // tidy_rewrite 4802
    std::vector<uint32_t> out;
    if (g.nan_minmax) nan_minmax_swap(words, g.nan_sat, nullptr);
    if (g.ldc_fold) { size_t k = xbg_spirv_ldc_fold(words.data(), words.size(), out); if (k) words.swap(out); }
    if (g.forward_temps) { size_t k = xbg_spirv_forward_temps(words.data(), words.size(), out); if (k) words.swap(out); }
    if (g.ldc_canon) { size_t m = 0, k = xbg_spirv_ldc_canon(words.data(), words.size(), out, &m); if (k) words.swap(out); }
    if (g.f2i_notrunc) { size_t k = xbg_spirv_f2i_notrunc(words.data(), words.size(), out); if (k) words.swap(out); }
    if (g.texcoord_unguard) { size_t k = xbg_spirv_texcoord_unguard(words.data(), words.size(), out); if (k) words.swap(out); }
    sl_ubovec(words, key);
}
void sl_idvs(std::vector<uint32_t> &w, uint32_t stage, const char *key) {   // idvs_rewrite 4745
    if (!g.idvs || stage != 0) return;
    std::vector<uint32_t> out; XbgIdvsStats st;
    if (!xbg_spirv_idvs_strip(w.data(), w.size(), out, &st)) return;
    const char *bad = xbg_spirv_idvs_check(w.data(), w.size(), out.data(), out.size());
    if (*bad) { RED("DM_NR_IDVS=%d check: vs %s: %s: the module is used as it was", g.idvs, key, bad); return; }
    if (g.idvs == 2) return;
    w.swap(out);
}
// The chain of load_shader_key / bundle_words, from the words as read (after spv-override) to the words of the module. gap: the soft /
// hard capability check between gather_snap and quad (module_gap in load_shader_key; bundle_words skips it).
void chain(std::vector<uint32_t> &w, uint32_t stage, const char *key, std::string *soft, std::string *hard) {
    const std::vector<uint32_t> read = w;   // the spv identity of the lone-lane list (pgv2 note_sha: words as read)
    sl_gscull(w, stage, key); sl_lone(w, stage, key, read); sl_early(w, stage); sl_invariant(w, stage); sl_gather_ext(w, key); sl_gather_snap(w, stage);
    if (soft && hard) caps(w, stage, *soft, *hard);
    sl_quad(w, stage); sl_warp(w, stage, key); sl_tidy(w, key); sl_idvs(w, stage, key);
}

// ---------------------------------------------------------------- after the rewrites
std::map<uint32_t, uint32_t> uniform_extent(const std::vector<uint32_t> &w) {   // spirv_uniform_extent 4268
    std::map<uint32_t, uint32_t> binding, constant, stride, var_type, result; std::set<uint32_t> uniform_vars, full; std::map<uint32_t, uint32_t> max_index;
    std::map<uint32_t, uint32_t> pointer_pointee, struct_member0;
    for (size_t i = 5; i < w.size();) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535; if (!n || i + n > w.size()) return {};
        switch (op) {
        case 71: if (n >= 4 && w[i + 2] == 33) binding[w[i + 1]] = w[i + 3]; if (n >= 4 && w[i + 2] == 6) stride[w[i + 1]] = w[i + 3]; break;
        case 43: if (n >= 4) constant[w[i + 2]] = w[i + 3]; break;
        case 30: if (n >= 3) struct_member0[w[i + 1]] = w[i + 2]; break;
        case 32: if (n >= 4) pointer_pointee[w[i + 1]] = w[i + 3]; break;
        case 59: if (n >= 4 && w[i + 3] == 2) { uniform_vars.insert(w[i + 2]); var_type[w[i + 2]] = w[i + 1]; } break;
        default: break;
        }
        i += n;
    }
    for (size_t i = 5; i < w.size();) {
        uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (op == 59) { i += n; continue; }
        if (op == 5 || op == 6 || op == 15 || op == 71 || op == 72 || op == 332 || op == 5632) { i += n; continue; }
        for (uint32_t k = 1; k < n; k++) {
            uint32_t id = w[i + k]; if (!uniform_vars.count(id)) continue;
            bool ok = (op == 65 || op == 66) && k == 3 && n >= 6;
            if (ok) { auto c = constant.find(w[i + 5]); auto m = constant.find(w[i + 4]); if (c == constant.end() || m == constant.end() || m->second != 0) ok = false; else max_index[id] = std::max(max_index[id], c->second); }
            if (!ok) full.insert(id);
        }
        i += n;
    }
    for (uint32_t v : uniform_vars) {
        auto b = binding.find(v); if (b == binding.end() || full.count(v)) continue;
        auto pt = pointer_pointee.find(var_type[v]); if (pt == pointer_pointee.end()) continue; auto m0 = struct_member0.find(pt->second); if (m0 == struct_member0.end()) continue;
        auto st = stride.find(m0->second); if (st == stride.end() || st->second != 16) continue;
        auto mi = max_index.find(v); result[b->second] = mi == max_index.end() ? 16 : (mi->second + 1) * 16;
    }
    return result;
}
constexpr uint32_t BIND_UNREAD = 1u << 29, BIND_SCANNED = 1u << 28;   // xbg_nr_ubodyn.inc XBG_BIND_UNREAD / _SCANNED
void ubd_mark(std::vector<xbg_shader_binding> &bs, const std::vector<uint32_t> &words, const char *key, uint32_t stage) {   // nrubd::mark
    if (!g.ubd_scan) return;
    std::vector<uint32_t> bn; std::vector<size_t> at;
    for (size_t i = 0; i < bs.size(); i++) { auto &b = bs[i]; if (b.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue; b.flags &= ~(BIND_UNREAD | BIND_SCANNED); bn.push_back(b.binding); at.push_back(i); }
    if (bn.empty()) return;
    std::vector<uint8_t> rd(bn.size());
    const int why = nr_spv_binding_reads(words.data(), words.size(), bn.data(), unsigned(bn.size()), rd.data());
    if (why != NRU_OK) RED("[nr] ubodyn: shader %s stage=%u: SPIR-V not understood (%s): its %zu uniform blocks stay at fixed offsets", key, stage, nru_why_name(why), bn.size());
    for (size_t k = 0; k < at.size(); k++) { auto &b = bs[at[k]]; b.flags |= BIND_SCANNED; if (!rd[k]) b.flags |= BIND_UNREAD; }
}
// bundle_module 5084: the optimizer's copy when the cache has one (and it reads no less, NR-08b), else the words; under the module lock.
VkResult make_module(const std::vector<uint32_t> &w, VkShaderModule *m, const std::string &key) {
    std::vector<uint32_t> opt;
    bool use = g.spv_opt > 0 && xbg_spvopt_lookup(w.data(), w.size(), opt, key.c_str());
    if (use && g.ubd_scan && !nr_spv_reads_kept(w.data(), w.size(), opt.data(), opt.size())) {
        RED("[nr] ubodyn: module %s: the optimized copy reads fewer bindings than the module that was scanned: %s", key.c_str(), g.ubd_ro ? "the original is created" : "(dry run: the copy is used)");
        use = !g.ubd_ro;
    }
    const std::vector<uint32_t> &code = use ? opt : w;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = code.size() * 4; ci.pCode = code.data();
    VkResult r; vkn_shlib_create_lock(); r = vkCreateShaderModule(g.c.device, &ci, nullptr, m); vkn_shlib_create_unlock();
    if (r != VK_SUCCESS && use) { xbg_spvopt_reject(w.data(), w.size(), key.c_str()); ci.codeSize = w.size() * 4; ci.pCode = w.data(); vkn_shlib_create_lock(); r = vkCreateShaderModule(g.c.device, &ci, nullptr, m); vkn_shlib_create_unlock(); }
    if (r != VK_SUCCESS) { ++st_mod_fail; *m = VK_NULL_HANDLE; RED("vkCreateShaderModule %d for %s", int(r), key.c_str()); }
    return r;
}

// ---------------------------------------------------------------- shaders
// [DOS2] VK-NATIVE stage 5.1 piece O2 (vkn_shlib_fs_facts, vkn_query.c DM_VK_NATIVE_GBUF_LE): what a fragment module's final words
// can do to the depth a draw leaves. Bits VKN_SHLIB_FS_*: OpKill (252) / OpTerminateInvocation (4416) / OpDemoteToHelperInvocation
// (5380); a BuiltIn FragDepth (22) / SampleMask (20) decoration (OpDecorate 71 / OpMemberDecorate 72, decoration BuiltIn = 11): the
// written depth / coverage is not the rasterised one. A malformed stream sets every bit (= "may discard").
uint32_t fs_facts_of(const std::vector<uint32_t> &w) {
    uint32_t f = 0;
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > w.size()) return 7u;
        if (op == 252 || op == 4416 || op == 5380) f |= 1u;
        const size_t d = op == 71 ? 2 : op == 72 ? 3 : 0;   // index of the decoration word
        if (d && n > d + 1 && w[i + d] == 11) { if (w[i + d + 1] == 22) f |= 2u; else if (w[i + d + 1] == 20) f |= 4u; }
        i += n;
    }
    return f;
}
// [DOS2] VK-NATIVE stage 5.1 G (vkn_shlib_vs_vindex, vkn_shader_vindex, vkn_draw.c's vertex fold): 1 = a vertex module's final
// words read BuiltIn VertexId (5) / VertexIndex (42) / BaseVertex (4424) - an OpDecorate (71) / OpMemberDecorate (72) BuiltIn (11) with
// one of them, the translator's NR-09 scan (xbg_game.cpp load_shader_key "vi") word for word. Those values include vertexOffset /
// firstVertex in Vulkan, which the fold changes; InstanceIndex / BaseInstance / DrawIndex do not depend on them. A malformed stream
// answers 1 (no fold).
uint32_t vs_vindex_of(const std::vector<uint32_t> &w) {
    for (size_t i = 5; i < w.size();) {
        const uint32_t n = w[i] >> 16, op = w[i] & 65535;
        if (!n || i + n > w.size()) return 1u;
        if (op == 71 && n >= 4 && w[i + 2] == 11 && (w[i + 3] == 42 || w[i + 3] == 5 || w[i + 3] == 4424)) return 1u;
        if (op == 72 && n >= 5 && w[i + 3] == 11 && (w[i + 4] == 42 || w[i + 4] == 5 || w[i + 4] == 4424)) return 1u;
        i += n;
    }
    return 0u;
}
struct Sh {
    std::string key; uint32_t stage = 0; VkShaderModule module = VK_NULL_HANDLE; std::string unusable;
    std::vector<xbg_shader_binding> bindings; std::vector<uint32_t> inputs, input_classes; std::vector<uint8_t> c1;
    int32_t fs_facts = -1;         // [DOS2] stage 5.1 O2: fragment: fs_facts_of(final words), set once at load; -1 = other stage
    int32_t vs_vindex = -1;        // [DOS2] stage 5.1 G: vertex: vs_vindex_of(final words), set once at load; -1 = other stage
    std::vector<uint32_t> words;   // vertex: the final words while DM_GFX_VTX_RETYPE is on (the "#vi" variants start from them)
    std::unique_ptr<nr_cv_prog> pos;   // [stage 5.2 B1] vertex, g.pos_on: the position program of the final words (only when ok)
    int64_t fs_out = -1;               // [stage 5.2 B1] fragment, g.pos_on: Output locations of the final words (bit = location), -1 = not built
    int8_t fs_noout = -1;              // [stage 5.3 V2] fragment, g.noout: 1 = no colour output and no sample-mask write (xbg_game.cpp draw's
                                       // !fs->raster.outputs && !fs->raster.sample_mask), 0 = writes colour, -1 = not built
    std::map<uint64_t, VkShaderModule> retyped, alpha;
    // [stage 5 technique A] DM_VK_NATIVE_COND 5 / 6: per Uniform block (Binding), the 16-byte elements the final words read
    // (vkn_occ_spv_read_mask); kind 1 = bits, 0 = the whole block, set once at load
    struct Rm { uint32_t binding; int kind; std::vector<uint64_t> bits; };
    std::vector<Rm> rmask;
};
std::unordered_map<std::string, std::unique_ptr<Sh>> g_sh;
std::set<std::string> g_missing_told;

// The bundle's words again, after the same chain (bundle_words 5362: no capability check). false = no bundle / malformed.
bool words_of(const std::string &key, std::vector<uint32_t> &w, uint32_t &stage) {
    std::vector<uint8_t> b; bool from_pack = false;
    if (!read_bundle(key, b, from_pack) || b.size() < 32 || b.size() > 16 * 1024 * 1024) return false;
    xbg_shader_header h; memcpy(&h, b.data(), sizeof h); if (h.magic != 0x31534758 || h.version != 1 || h.spirv_bytes % 4) return false;
    size_t off = sizeof h + size_t(h.binding_count) * sizeof(xbg_shader_binding) + size_t(h.input_count) * 4 + h.c1_bytes; if (off + h.spirv_bytes != b.size()) return false;
    w.resize(h.spirv_bytes / 4); memcpy(w.data(), b.data() + off, h.spirv_bytes); spv_override(key, w);
    chain(w, h.stage, key.c_str(), nullptr, nullptr);
    stage = h.stage; return true;
}
// The missing stage as an offline job record (0x38 zero bytes + code + control; tools/dos2_shader_jobs.py) in <shaders>/../missing.
void dump_missing(const std::string &key, const void *control, const void *code) {
    if (!code || !control) return;
    const uint8_t *ct = (const uint8_t *)control, *cd = (const uint8_t *)code; uint32_t nc = 0; memcpy(&nc, ct + 0x704, 4);
    const bool gfx = nc >= 4 && cd[0] == 0x78 && cd[1] == 0x56 && cd[2] == 0x34 && cd[3] == 0x12; const size_t nctl = gfx ? 0x880 : 0xe48;
    if (!nc || nc >= (1u << 24)) return;
    const std::string dir = g.dir + "/../missing"; mkdir(dir.c_str(), 0775);
    FILE *o = fopen((dir + "/" + key + ".rec").c_str(), "wb"); if (!o) return;
    static const uint8_t z[0x38] = {}; fwrite(z, 1, sizeof z, o); fwrite(cd, 1, nc, o); fwrite(ct, 1, nctl, o); fclose(o);
    L("missing shader %s dumped to %s/%s.rec (code %u bytes, %s)", key.c_str(), dir.c_str(), key.c_str(), nc, gfx ? "graphics" : "compute");
}
// load_shader_key 5103 for mode 1 (under g_mu). NULL = no bundle (or a malformed one: the translator aborted there).
Sh *load(const std::string &key, const void *control, const void *code) {
    auto found = g_sh.find(key); if (found != g_sh.end()) return found->second.get();
    const uint64_t t0 = now_ns();
    std::vector<uint8_t> bytes; bool from_pack = false; const bool have = read_bundle(key, bytes, from_pack);
    if (!have) {
        if (g_missing_told.insert(key).second) { ++st_missing; L("missing shader bundle %s: its draws are skipped (%zu missing so far)", key.c_str(), g_missing_told.size()); dump_missing(key, control, code); }
        return nullptr;
    }
    (from_pack ? st_pack : st_file)++;
    xbg_shader_header h{};
    const bool len_ok = bytes.size() >= 32 && bytes.size() <= 16 * 1024 * 1024; if (len_ok) memcpy(&h, bytes.data(), sizeof h);
    const bool head_ok = len_ok && h.magic == 0x31534758 && h.version == 1 && h.stage < 6 && h.binding_count <= 256 && h.input_count <= 16 && h.c1_bytes <= 65536 && h.spirv_bytes % 4 == 0;
    size_t offset = sizeof h;
    if (!head_ok || offset + size_t(h.binding_count) * sizeof(xbg_shader_binding) + size_t(h.input_count) * 4 + h.c1_bytes + h.spirv_bytes != bytes.size()) {
        RED("shader bundle %s: %s (the translator aborted here): its draws are skipped", key.c_str(), !len_ok ? "invalid length" : !head_ok ? "invalid header" : "extent mismatch");
        ++st_missing; return nullptr;
    }
    auto p = std::make_unique<Sh>(); p->key = key; p->stage = h.stage;
    p->bindings.resize(h.binding_count); memcpy(p->bindings.data(), bytes.data() + offset, h.binding_count * sizeof(xbg_shader_binding)); offset += h.binding_count * sizeof(xbg_shader_binding);
    p->inputs.resize(h.input_count); memcpy(p->inputs.data(), bytes.data() + offset, h.input_count * 4); offset += h.input_count * 4;
    p->c1.assign(bytes.begin() + long(offset), bytes.begin() + long(offset + h.c1_bytes)); offset += h.c1_bytes;
    std::vector<uint32_t> words(h.spirv_bytes / 4); memcpy(words.data(), bytes.data() + offset, h.spirv_bytes); spv_override(key, words);
    std::string soft, hard; chain(words, h.stage, key.c_str(), &soft, &hard);
    if (!hard.empty()) { p->unusable = hard; ++st_gaps_hard; RED("shader %s stage=%u: %s (the translator aborted here): no module, its draws / dispatches are skipped", key.c_str(), h.stage, hard.c_str()); }
    else if (!soft.empty()) { p->unusable = soft; L("shader %s stage=%u needs %s, which this device lacks: no module, its draws/dispatches are skipped", key.c_str(), h.stage, soft.c_str()); }
    {   // the uniform-extent trim of the binding list
        auto extent = uniform_extent(words);
        for (auto &b : p->bindings) if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source == XBG_SOURCE_UNIFORM) { auto e = extent.find(b.binding); uint32_t declared = b.bytes ? b.bytes : 65536; if (e != extent.end() && e->second < declared) b.bytes = e->second; }
    }
    if (h.stage == 0) {
        p->input_classes.resize(p->inputs.size());
        if (xbg_vertex_input_classes(words.data(), words.size(), p->inputs.data(), p->inputs.size(), p->input_classes.data()) != 0 && p->unusable.empty()) {
            p->unusable = "unsupported vertex input scalar interface"; ++st_gaps_hard; RED("shader %s: unsupported vertex input scalar interface (the translator aborted here): no module", key.c_str());
        }
        if (g.retype_keep) p->words = words;
    }
    ubd_mark(p->bindings, words, key.c_str(), h.stage);
    if (h.stage == 4) p->fs_facts = int32_t(fs_facts_of(words));   // [DOS2] stage 5.1 O2: the words the module is made of
    // [DOS2] stage 5.1 G: the base module's words; the "#vi" retype variants are made from these same words by xbg_vertex_retype,
    // which rewrites input variables / loads only (it adds no BuiltIn), so the answer holds for every module of this shader
    if (h.stage == 0) p->vs_vindex = int32_t(vs_vindex_of(words));
    // [stage 5.2 B1] DM_VK_NATIVE_LOAD (vkn_pass.c, the cover proof of a pass's first draw): the position program of the final words
    // (xbg_spirv_pos_prog, NR-10b: 117 of DOS2's vertex shaders have one) and the fragment output locations (xbg_spirv_raster_facts)
    if (g.pos_on && h.stage == 0) { nr_cv_prog pr; xbg_spirv_pos_prog(words.data(), words.size(), &pr); if (pr.ok) p->pos.reset(new nr_cv_prog(pr)); }
    if ((g.pos_on || g.noout) && h.stage == 4) {   // [stage 5.3 V2] one scan for both facts (built only where a switch asks: never on Adreno)
        const XbgRasterFacts rf = xbg_spirv_raster_facts(words.data(), words.size(), 4);
        if (g.pos_on) p->fs_out = int64_t(rf.outputs);
        if (g.noout) p->fs_noout = int8_t(!rf.outputs && !rf.sample_mask ? 1 : 0);
    }
    {   // [stage 5 technique A] the read masks (only when technique A is asked for: DM_VK_NATIVE_COND 5 / 6)
        static const bool occ_on = [] { const char *e = getenv("DM_VK_NATIVE_COND"); return e && (atoi(e) == 5 || atoi(e) == 6); }();
        if (occ_on && (h.stage == 0 || h.stage == 4))
            for (auto &b : p->bindings) if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source == XBG_SOURCE_UNIFORM) {
                Sh::Rm m; m.binding = b.binding; m.bits.assign(((b.bytes ? b.bytes : 65536u) / 16u + 63u) / 64u, 0);
                m.kind = vkn_occ_spv_read_mask(words.data(), words.size(), b.binding, m.bits.data(), uint32_t(m.bits.size())) == 1 ? 1 : 0;
                if (!m.kind) m.bits.clear();
                p->rmask.push_back(std::move(m));
            }
    }
    if (p->unusable.empty() && make_module(words, &p->module, key) != VK_SUCCESS) p->unusable = "vkCreateShaderModule failed";
    if (!p->unusable.empty()) ++st_unusable;
    Sh *out = p.get(); g_sh.emplace(key, std::move(p)); ++st_loads; st_ns += now_ns() - t0;
    return out;
}
// alpha_module 8182
VkShaderModule alpha_variant(Sh &s, uint32_t func, float ref) {
    uint32_t bits; memcpy(&bits, &ref, 4); const uint64_t k = (uint64_t(func) << 32) | bits;
    auto it = s.alpha.find(k); if (it != s.alpha.end()) return it->second;
    VkShaderModule m = s.module; const char *why = nullptr; std::vector<uint32_t> w, out; uint32_t stage = 0; xbg_spirv_alpha_info info;
    if (!words_of(s.key, w, stage) || stage != 4) why = "bundle reload failed";
    else why = xbg_spirv_alpha_test(w.data(), w.size(), func, ref, out, &info);
    if (!why && func == 1 && g.ubd_ro) RED("[nr] ubodyn: shader %s: alpha NEVER variant under the read-only rule", s.key.c_str());
    if (!why) {
        char tag[24]; snprintf(tag, sizeof tag, "#at%u_%08x", func, bits); VkShaderModule v = VK_NULL_HANDLE;
        if (make_module(out, &v, s.key + tag) == VK_SUCCESS) { m = v; ++st_alpha; } else why = "vkCreateShaderModule failed";
    }
    if (m == s.module) { const uint64_t n = ++st_alpha_ref; if (n <= 16 || !(n & (n - 1))) L("alpha test func %u ref=%g on %s not applied: %s; drawn without the test (%llu so far)", func, double(ref), s.key.c_str(), why, (unsigned long long)n); }
    s.alpha.emplace(k, m); return m;
}
// vtx_retype_module 8112
VkShaderModule retype_variant(Sh &s, uint64_t code64) {
    auto it = s.retyped.find(code64); if (it != s.retyped.end()) return it->second;
    const uint32_t code = uint32_t(code64), conv = uint32_t(code64 >> 32);
    std::vector<uint32_t> loc, want;
    for (uint32_t l = 0; l < 16; l++) { const uint32_t cv = (conv >> (2 * l)) & 3, rc = (code >> (2 * l)) & 3; if (!cv && !rc) continue; loc.push_back(l); want.push_back(cv == 1 ? XBG_VTX_WANT_SINT_TO_FLOAT : cv ? XBG_VTX_WANT_UINT_TO_FLOAT : rc - 1); }
    std::vector<uint32_t> out, reloaded; std::string why; VkShaderModule m = s.module; uint32_t stage = 0;
    char tag[32]; if (conv) snprintf(tag, sizeof tag, "#vi%08xc%08x", code, conv); else snprintf(tag, sizeof tag, "#vi%08x", code);
    if (s.words.empty() && (!words_of(s.key, reloaded, stage) || stage != 0)) why = "bundle reload failed";
    else if (xbg_vertex_retype(s.words.empty() ? reloaded : s.words, loc.data(), loc.size(), want.data(), out, &why)) {
        VkShaderModule v = VK_NULL_HANDLE;
        if (make_module(out, &v, s.key + tag) == VK_SUCCESS) { m = v; ++st_retype; } else why = "vkCreateShaderModule failed";
    }
    if (m == s.module) { const uint64_t n = ++st_retype_ref; if (n <= 16 || !(n & (n - 1)) || conv) L("vertex input retype %s code=%s refused: %s; the base module is kept%s", s.key.c_str(), tag + 3, why.c_str(), conv ? " (WRONG PICTURE: an integer attribute feeds a float input)" : ""); }
    s.retyped.emplace(code64, m); return m;
}

// ---------------------------------------------------------------- the switches (xbg_game_initialize 9941-10074, d3perf_switches 9850)
void d3perf(uint32_t timestamp_bits) {
    static const char *const env[XBG_ST_COUNT] = {"DM_GFX_REPORT_INPASS", "DM_GFX_LAZY_CLEAR", "DM_GFX_NAN_SAT", "DM_GFX_LDC_CANON", "DM_GFX_F2I_NOTRUNC"};
    auto set = [](const char *e) { const char *v = getenv(e); return v && *v; };
    auto on = [](const char *e) { const char *v = getenv(e); return v && *v && atoi(v) != 0; };
    const bool defaults = on("DM_GFX_D3PERF"), adreno = g.props.vendorID == 0x5143;
    const char *st = getenv("DM_GFX_SELFTEST"); const int mode = st && *st ? atoi(st) : -1;
    bool given[XBG_ST_COUNT], need = false; for (int k = 0; k < XBG_ST_COUNT; k++) { given[k] = set(env[k]); need = need || (defaults && !given[k]); }
    const bool run = need && (adreno ? mode == 1 : mode != 0);
    XbgSelftestOut so;
    if (run) {
        XbgSelftestIn in; in.physical = g.c.physical; in.device = g.c.device; in.queue = g.c.queue; in.queue_family = g.c.queue_family;
        in.precise_occlusion = g.core.occlusionQueryPrecise != VK_FALSE; in.timestamp_bits = timestamp_bits;
        in.force_fail = xbg_selftest_parse(getenv("DM_GFX_SELFTEST_FORCE_FAIL")); in.nan_swap = selftest_nan_swap; xbg_selftest_run(in, so);
    }
    bool value[XBG_ST_COUNT];
    for (int k = 0; k < XBG_ST_COUNT; k++) value[k] = given[k] ? on(env[k]) : defaults && (run ? so.pass[k] != 0 : adreno);
    g.nan_sat = value[XBG_ST_NAN_SAT]; g.ldc_canon = value[XBG_ST_LDC_CANON]; g.f2i_notrunc = value[XBG_ST_F2I_NOTRUNC];
    L("d3-perf shader switches: self-test %s; DM_GFX_NAN_SAT=%d DM_GFX_LDC_CANON=%d DM_GFX_F2I_NOTRUNC=%d", run ? (so.ran ? "ran" : "did not run") : "not run", int(g.nan_sat), int(g.ldc_canon), int(g.f2i_notrunc));
}
} // namespace

extern "C" void vkn_b5_install(uint32_t vendor, const char *device_name) __attribute__((weak));   // [stage 5.2 B5 / B4] vkn_mat.c (it installs vkn_prepass.c too)
extern "C" unsigned dm_vkn_nonvn_mask(void) __attribute__((weak));   // hle_dos2_vkn_nonvn.c (reads its env itself: valid before the install)
// Mode 1, DM_VK_NATIVE_SHLIB not 0, and the no-NVN device start (DM_VK_NATIVE_NONVN step B): without B, HLE NVN answers the device start
// and its bodies reach the translator, which then must be initialised (the old doors are used).
extern "C" int vkn_shlib_on(void) {
    static const int on = [] {
        const char *e = getenv("DM_VK_NATIVE"), *s = getenv("DM_VK_NATIVE_SHLIB");
        return e && atoi(e) == 1 && !(s && *s && atoi(s) == 0) && (!dm_vkn_nonvn_mask || (dm_vkn_nonvn_mask() & 1u)) ? 1 : 0;
    }();
    return on;
}
extern "C" int vkn_shlib_ready(void) { return g.ready ? 1 : 0; }
static std::atomic<uint64_t> g_entered[VKN_SHLIB_X_N];
extern "C" void vkn_shlib_translator_entered(int which) {
    if (which < 0 || which >= VKN_SHLIB_X_N || !vkn_shlib_on()) return;
    if (!g_entered[which].fetch_add(1, std::memory_order_relaxed)) RED("translator entry point %s reached although the translator is not initialised (DM_VK_NATIVE_SHLIB)", vkn_shlib_translator_entry_name(which));
}
extern "C" uint64_t vkn_shlib_translator_calls(uint64_t out[VKN_SHLIB_X_N]) {
    uint64_t t = 0; for (int i = 0; i < VKN_SHLIB_X_N; i++) { const uint64_t n = g_entered[i].load(std::memory_order_relaxed); if (out) out[i] = n; t += n; } return t;
}
extern "C" const char *vkn_shlib_translator_entry_name(int which) {
    static const char *const n[VKN_SHLIB_X_N] = {"xbg_game_initialize", "xbg_vkn_shader", "xbg_vkn_alpha_module", "xbg_vkn_retype_module", "xbg_vkn_cache_dir",
                                                 "xbg_vkn_lib_report", "xbg_submit", "xbg_present_texture", "xbg_game_shutdown", "statistics (xbg_nr_mem_usage / _trim)"};
    return which >= 0 && which < VKN_SHLIB_X_N ? n[which] : "?";
}

extern "C" int vkn_shlib_init(const vkn_shlib_config *c, char *error, size_t error_bytes) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g.ready) { if (error && error_bytes) snprintf(error, error_bytes, "shader library already initialised"); return -1; }
    if (!c || !c->physical || !c->device || !c->shader_directory) { if (error && error_bytes) snprintf(error, error_bytes, "invalid shader library config"); return -1; }
    g.c = *c; g.dir = c->shader_directory; g.c.shader_directory = nullptr;
    { size_t cut = g.dir.find_last_of('/'); g.base = cut == std::string::npos ? g.dir : g.dir.substr(0, cut); }
    // device facts, as the translator's initialise (9914-9945, 10111)
    vkGetPhysicalDeviceProperties(g.c.physical, &g.props);
    VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; g.subgroup = VkPhysicalDeviceSubgroupProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    pp.pNext = &g.subgroup; vkGetPhysicalDeviceProperties2(g.c.physical, &pp); g.subgroup.pNext = nullptr;
    g.core = g.c.enabled_core;
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(g.c.physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn); if (qn) vkGetPhysicalDeviceQueueFamilyProperties(g.c.physical, &qn, qf.data());
    const uint32_t timestamp_bits = g.c.queue_family < qn ? qf[g.c.queue_family].timestampValidBits : 0;
    { char unknown[32]; const char *se = getenv("DM_GFX_SIM_GPU"); g.sim = xbg_sim_parse(se, unknown, sizeof unknown);
      g.family = xbg_gpu_family(g.props.vendorID, g.props.deviceName);
      if (g.sim) { xbg_sim_features(g.sim, &g.core); xbg_sim_subgroup(g.sim, &g.subgroup); xbg_sim_limits(g.sim, &g.props.limits); }
      g.policy = xbg_effective_family(g.family, g.sim); }
    { uint32_t n = 0; vkEnumerateDeviceExtensionProperties(g.c.physical, nullptr, &n, nullptr); std::vector<VkExtensionProperties> ext(n); if (n) vkEnumerateDeviceExtensionProperties(g.c.physical, nullptr, &n, ext.data());
      g.ffl2 = g.props.apiVersion >= VK_API_VERSION_1_3; for (auto &e : ext) if (!strcmp(e.extensionName, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME)) g.ffl2 = true; }
    // switches (same names, defaults and order of precedence)
    g.serial = xbg_env_or(getenv("DM_GFX_PIPE_SERIAL"), xbg_pipe_serial_default(g.policy));
    {   // NR-08b (nrubd::init with the family's dynamic-UBO default)
        const char *e = getenv("DM_NR_UBODYN"); int m = e && *e ? atoi(e) : -1; if (m == 3) m = 0; if (m < -1 || m > 3) m = 0;
        const char *u = getenv("DM_GFX_UBO_DYNAMIC"); const int uenv = u && *u ? atoi(u) : -1;
        g.ubd_ro = xbg_nr_ubodyn_active(m, uenv, xbg_ubo_dynamic_default(g.policy), g.policy);
        g.ubd_mode = m < 0 ? xbg_nr_ubodyn_default(g.policy) : m; if (g.ubd_ro && g.ubd_mode != 1) g.ubd_mode = 1;
        g.ubd_scan = g.ubd_mode == 1 || g.ubd_mode == 2;
    }
    {   // quad / warp32 (Adreno 740-799 and the 8xx policy: off unless a marker), then the rest
        const bool fault = xbg_adreno_rewrites_fault(g.props.vendorID, g.props.deviceName);
        const char *a8e = getenv("DM_GFX_ADRENO8_POLICY"); if (!a8e || !*a8e) a8e = "0";
        const bool a8 = xbg_adreno8_policy(g.props.vendorID, g.props.deviceName, a8e);
        auto sw = [&](const char *marker, const char *env) { FILE *q = fopen((g.base + marker).c_str(), "rb"); const bool present = q != nullptr; int ch = -1; if (q) { ch = fgetc(q); fclose(q); } return xbg_adreno_rewrite_option(present, ch, getenv(env), fault || a8); };
        g.quad = sw("/gfx-quad-derivatives", "DM_GFX_QUAD_DERIVATIVES");
        if (g.quad && !(g.subgroup.subgroupSize >= 4 && (g.subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) && (g.subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT))) g.quad = false;
        g.warp32 = sw("/gfx-warp32", "DM_GFX_WARP32");
        auto dm = [&](const char *marker, const char *env) { FILE *q = fopen((g.base + marker).c_str(), "rb"); bool on = false; if (q) { on = fgetc(q) != '0'; fclose(q); } else if (const char *e = getenv(env)) on = atoi(e) != 0; return on; };
        g.ldc_fold = dm("/gfx-ldc-fold", "DM_GFX_LDC_FOLD"); g.forward_temps = dm("/gfx-forward-temps", "DM_GFX_FORWARD_TEMPS"); g.nan_minmax = dm("/gfx-nan-minmax", "DM_GFX_NAN_MINMAX");
    }
    d3perf(timestamp_bits);
    { const char *e = getenv("DM_GFX_TEXCOORD_NAN"); g.texcoord_unguard = !(e && *e ? atoi(e) != 0 : true); }
    { const char *e = getenv("DM_NR_UBOVEC"); g.ubovec = e && *e ? atoi(e) : xbg_nr_ubovec_default(g.policy); if (g.ubovec < 0 || g.ubovec > 3) g.ubovec = 0; }
    { const char *e = getenv("DM_NR_IDVS"); g.idvs = e && *e ? atoi(e) : xbg_nr_idvs_default(g.policy); if (g.idvs < 0 || g.idvs > 2) g.idvs = 0; }
    { const char *e = getenv("DM_GFX_ALPHA_TEST"); g.alpha = e && *e ? atoi(e) != 0 : true; }
    { const char *e = getenv("DM_GFX_EARLY_Z"); g.early_z = e ? atoi(e) != 0 : 1; }
    { const char *e = getenv("DM_GFX_INVARIANT"); g.invariant = e ? atoi(e) != 0 : 0; }
    { const char *e = getenv("DM_GFX_GSCULL_FIX"); g.gscull = e ? int(strtol(e, nullptr, 0)) : 1 | 32 | 64; }
    { const char *e = getenv("DM_GFX_GATHER_SNAP"); g.gather_snap = e && *e ? atoi(e) : 1; }
    { const char *e = getenv("DM_GFX_WARP_MODE"); g.warp_mode = e && !strcmp(e, "native") ? 0 : e && !strcmp(e, "lone") ? 1 : g.subgroup.subgroupSize == 32 ? 0 : 1; }
    { const char *e = getenv("DM_GFX_VTX_RETYPE"); g.retype_keep = e && *e ? atoi(e) != 0 : g.props.vendorID != 0x5143; }
    { const char *e = getenv("DM_GFX_SPV_OPT"); g.spv_opt = e && *e ? atoi(e) : 0; if (g.spv_opt > 0) xbg_spvopt_init((g.base + "/spvopt").c_str(), g.spv_opt); }
    g.pos_on = vkn_policy_load(int(g.policy)) != 0;   // [stage 5.2 B1] (the same reader vkn_pass resolves its switch with)
    g.noout = vkn_policy_env_or("DM_GFX_NOOUT_MASK", vkn_policy_noout_mask_default(int(g.policy)), nullptr) != 0;   // [stage 5.3 V2] (vkn_drv.c reads it the same way)
    g.ready = true;
    L("initialised (DM_VK_NATIVE_SHLIB: the translator is not initialised): gpu \"%s\" vendor %#x family %s policy %s sim %#x; subgroup %u stages %#x ops %#x; "
      "rewrites: quad %d warp32 %d ldc_fold %d forward_temps %d nan_minmax %d nan_sat %d ldc_canon %d f2i_notrunc %d texcoord_unguard %d ubovec %d idvs %d early_z %d "
      "invariant %d gather_snap %d gscull %#x; alpha test %d, retype words kept %d, spv opt %d, NR-08b scan %d ro %d, module lock %d, format_feature_flags2 %d",
      g.props.deviceName, g.props.vendorID, xbg_gpu_family_name(g.family), xbg_gpu_family_name(g.policy), g.sim, g.subgroup.subgroupSize, g.subgroup.supportedStages,
      g.subgroup.supportedOperations, int(g.quad), int(g.warp32), int(g.ldc_fold), int(g.forward_temps), int(g.nan_minmax), int(g.nan_sat), int(g.ldc_canon),
      int(g.f2i_notrunc), int(g.texcoord_unguard), g.ubovec, g.idvs, g.early_z, g.invariant, g.gather_snap, unsigned(g.gscull), int(g.alpha), int(g.retype_keep),
      g.spv_opt, int(g.ubd_scan), int(g.ubd_ro), int(g.serial), int(g.ffl2));
    L("lone-lane for every stage with subgroup operations: %s (DM_GFX_WARP_MODE, subgroup size %u)", g.warp_mode ? "yes" : "no (native subgroups)", g.subgroup.subgroupSize);
    if (vkn_b5_install) vkn_b5_install(g.props.vendorID, g.props.deviceName);   // [stage 5.2 B5 / B4] MAT + XLQ and the pre-pass hybrid (vkn_mat.c / vkn_prepass.c): their chain functions (mode 1)
    return 0;
}

extern "C" int vkn_shlib_shader(const char *key, const void *control, const void *code, xbg_vkn_stage *out) {
    if (!key || !out) return -1;
    out->shader = nullptr; out->module = VK_NULL_HANDLE; out->binding_count = out->input_count = out->c1_bytes = 0; out->c1 = nullptr;
    std::lock_guard<std::mutex> lock(g_mu); if (!g.ready) return -1;
    Sh *s = load(std::string(key), control, code); if (!s) return 1;
    out->shader = s; out->module = s->unusable.empty() ? s->module : VK_NULL_HANDLE; out->stage = s->stage;
    out->binding_count = uint32_t(std::min<size_t>(s->bindings.size(), 256)); memcpy(out->bindings, s->bindings.data(), out->binding_count * sizeof(xbg_shader_binding));
    out->input_count = uint32_t(std::min<size_t>(s->inputs.size(), 16));
    for (uint32_t i = 0; i < out->input_count; i++) { out->inputs[i] = s->inputs[i]; out->input_classes[i] = i < s->input_classes.size() ? s->input_classes[i] : 0; }
    out->c1 = s->c1.empty() ? nullptr : s->c1.data(); out->c1_bytes = uint32_t(s->c1.size());
    return 0;
}
extern "C" VkShaderModule vkn_shlib_alpha_module(void *shader, uint32_t func, float ref) {
    if (!shader) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(g_mu); Sh &s = *static_cast<Sh *>(shader);
    if (!g.ready || !s.unusable.empty() || s.stage != 4 || func == 8 || !g.alpha) return s.module;
    return alpha_variant(s, func, ref);
}
// [DOS2] stage 5.1 O2: immutable after load (written before the object is published under g_mu): no lock.
extern "C" int vkn_shlib_fs_facts(const void *shader) {
    const Sh *s = static_cast<const Sh *>(shader);
    return s && s->stage == 4 ? s->fs_facts : -1;
}
// [DOS2] stage 5.1 G: immutable after load (as fs_facts): no lock.
extern "C" int vkn_shlib_vs_vindex(const void *shader) {
    const Sh *s = static_cast<const Sh *>(shader);
    return s && s->stage == 0 ? s->vs_vindex : -1;
}
// ==== [DOS2] VK-NATIVE stage 5.2 B1 (vkn_pass.c DM_VK_NATIVE_LOAD): facts of the final words for the cover proof; immutable after load
// (as fs_facts): no lock. The policy family (xbg_effective_family of init; -1 before init) for vkn_policy.h's getters.
extern "C" const nr_cv_prog *vkn_shlib_vs_pos(const void *shader) {
    const Sh *s = static_cast<const Sh *>(shader);
    return s && s->stage == 0 ? s->pos.get() : nullptr;
}
extern "C" int64_t vkn_shlib_fs_outputs(const void *shader) {
    const Sh *s = static_cast<const Sh *>(shader);
    return s && s->stage == 4 ? s->fs_out : -1;
}
extern "C" int vkn_shlib_policy(void) { return g.ready ? int(g.policy) : -1; }
// ==== [DOS2] VK-NATIVE stage 5.3 V2 (vkn_drv.c): the no-colour-output fact (immutable after load, as fs_facts: no lock) and the module
// lock's switch (set once at init).
extern "C" int vkn_shlib_fs_noout(const void *shader) {
    const Sh *s = static_cast<const Sh *>(shader);
    return s && s->stage == 4 ? s->fs_noout : -1;
}
extern "C" int vkn_shlib_serial(void) { return g.ready && g.serial ? 1 : 0; }
// ==== [/stage 5.3 V2]
// [stage 5 technique A] immutable after load (as fs_facts): no lock.
extern "C" int vkn_shlib_read_mask(const void *shader, uint32_t binding, const uint64_t **bits, uint32_t *nwords) {
    const Sh *s = static_cast<const Sh *>(shader);
    if (!s) return -1;
    for (const auto &m : s->rmask) if (m.binding == binding) { if (m.kind == 1) { *bits = m.bits.data(); *nwords = uint32_t(m.bits.size()); } return m.kind; }
    return -1;
}
// ==== [/stage 5.2 B1]
extern "C" VkShaderModule vkn_shlib_retype_module(void *shader, uint64_t code) {
    if (!shader) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(g_mu); Sh &s = *static_cast<Sh *>(shader);
    if (!g.ready || !s.unusable.empty() || s.stage != 0 || !code) return s.module;
    return retype_variant(s, code);
}
extern "C" int vkn_shlib_cache_dir(char *out, size_t bytes) {
    if (!out || !bytes) return -1;
    out[0] = 0;
    std::lock_guard<std::mutex> lock(g_mu); if (!g.ready) return -1;
    if (g.base.size() + 1 > bytes) return -1;
    memcpy(out, g.base.c_str(), g.base.size() + 1); return 0;
}
extern "C" void vkn_shlib_create_lock(void) { if (g.serial) g_create_mu.lock(); }
extern "C" void vkn_shlib_create_unlock(void) { if (g.serial) g_create_mu.unlock(); }
extern "C" int vkn_shlib_astc_key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) {
    if (!crc || !adler || (!mem && size)) return -1;
    const uint8_t *m = static_cast<const uint8_t *>(mem); uLong c = crc32(0L, Z_NULL, 0), a = adler32(0L, Z_NULL, 0);
    for (uint64_t o = 0; o < size;) { const uInt n = uInt(std::min<uint64_t>(size - o, 1u << 30)); c = crc32(c, m + o, n); a = adler32(a, m + o, n); o += n; }
    *crc = uint32_t(c); *adler = uint32_t(a); return 0;
}
extern "C" void vkn_shlib_stats_get(vkn_shlib_stats *o) {
    if (!o) return;
    *o = vkn_shlib_stats{st_loads.load(), st_missing.load(), st_unusable.load(), st_pack.load(), st_file.load(), st_native.load(), st_alpha.load(), st_alpha_ref.load(),
                         st_retype.load(), st_retype_ref.load(), st_mod_fail.load(), st_gaps_hard.load(), st_ns.load()};
}
extern "C" int vkn_shlib_report(char *out, size_t bytes) {
    if (!out || !bytes) return 0;
    vkn_shlib_stats s; vkn_shlib_stats_get(&s);
    return snprintf(out, bytes, "shader library %s: %llu shaders loaded (%llu from the pack, %llu from files, %llu native, %llu unusable, %llu missing), variants alpha %llu "
                    "(refused %llu) retype %llu (refused %llu), module failures %llu, %.1f ms loading; module lock %s",
                    g.ready ? "ready" : "NOT initialised", (unsigned long long)s.loads, (unsigned long long)s.from_pack, (unsigned long long)s.from_file,
                    (unsigned long long)s.native_used, (unsigned long long)s.unusable, (unsigned long long)s.missing, (unsigned long long)s.alpha_variants,
                    (unsigned long long)s.alpha_refused, (unsigned long long)s.retype_variants, (unsigned long long)s.retype_refused, (unsigned long long)s.module_fail,
                    s.ns / 1e6, g.serial ? "on" : "off");
}
// ==== [DOS2] VK-NATIVE stage 5.2 B5 / B4 (dm/gfx/vkn/vkn_mat.c, vkn_prepass.c; vkn_shlib.h block "stage 5.2 B5"): the words a technique's
// module is made from, its module, and a record of a pack next to the shader packs. Same steps as words_of / retype_variant /
// make_module / pack_get above (nothing new is decided here).
extern "C" int vkn_shlib_b5_words(void *shader, uint64_t code64, uint32_t **words, size_t *nwords, uint8_t **raw, size_t *nraw) {
    if (words) *words = nullptr; if (nwords) *nwords = 0; if (raw) *raw = nullptr; if (nraw) *nraw = 0;
    if (!shader || !words || !nwords) return -1;
    std::lock_guard<std::mutex> lock(g_mu); if (!g.ready) return -1;
    const Sh &s = *static_cast<const Sh *>(shader);
    if (!s.unusable.empty() || (code64 && s.stage != 0)) return 1;
    // words_of, keeping the bundle's own SPIR-V bytes (what the NR-33B gate records are proven on: xbg_nr_mat.inc note's `raw`)
    std::vector<uint8_t> b; bool from_pack = false;
    if (!read_bundle(s.key, b, from_pack) || b.size() < 32 || b.size() > 16 * 1024 * 1024) return 1;
    xbg_shader_header h; memcpy(&h, b.data(), sizeof h); if (h.magic != 0x31534758 || h.version != 1 || h.spirv_bytes % 4 || h.stage != s.stage) return 1;
    const size_t off = sizeof h + size_t(h.binding_count) * sizeof(xbg_shader_binding) + size_t(h.input_count) * 4 + h.c1_bytes; if (off + h.spirv_bytes != b.size()) return 1;
    std::vector<uint32_t> w(h.spirv_bytes / 4); memcpy(w.data(), b.data() + off, h.spirv_bytes); spv_override(s.key, w);
    chain(w, h.stage, s.key.c_str(), nullptr, nullptr);
    if (code64) {   // retype_variant's words (xbg_game.cpp vtx_retype_words)
        const uint32_t code = uint32_t(code64), conv = uint32_t(code64 >> 32); std::vector<uint32_t> loc, want, out; std::string why;
        for (uint32_t l = 0; l < 16; l++) { const uint32_t cv = (conv >> (2 * l)) & 3, rc = (code >> (2 * l)) & 3; if (!cv && !rc) continue; loc.push_back(l); want.push_back(cv == 1 ? XBG_VTX_WANT_SINT_TO_FLOAT : cv ? XBG_VTX_WANT_UINT_TO_FLOAT : rc - 1); }
        if (!xbg_vertex_retype(w, loc.data(), loc.size(), want.data(), out, &why)) return 1;
        w.swap(out);
    }
    uint32_t *wo = static_cast<uint32_t *>(malloc(w.size() * 4 + 4)); if (!wo) return 1;
    memcpy(wo, w.data(), w.size() * 4); *words = wo; *nwords = w.size();
    if (raw && nraw) { uint8_t *r = static_cast<uint8_t *>(malloc(h.spirv_bytes + 1)); if (r) { memcpy(r, b.data() + off, h.spirv_bytes); *raw = r; *nraw = h.spirv_bytes; } }
    return 0;
}
extern "C" VkShaderModule vkn_shlib_b5_module(const uint32_t *w, size_t n, const char *key) {
    if (!w || !n || !key || !g.ready) return VK_NULL_HANDLE;
    VkShaderModule m = VK_NULL_HANDLE; const std::vector<uint32_t> v(w, w + n);
    return make_module(v, &m, key) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
extern "C" int vkn_shlib_b5_record(const char *pack, const char *key, uint8_t **out, size_t *n) {
    if (out) *out = nullptr; if (n) *n = 0;
    if (!pack || !key || !out || !n) return -1;
    static std::mutex mu; static std::map<std::string, std::pair<int, std::vector<Ent>>> open;   // pack name -> (fd, index); fd -1 = none, -2 = damaged
    std::lock_guard<std::mutex> l(mu);
    auto it = open.find(pack);
    if (it == open.end()) {
        if (!g.ready) return -1;
        int fd = -1; std::vector<Ent> idx; bool damaged = false;
        const bool ok = pack_open(g.dir + "/" + pack, fd, idx, damaged);
        it = open.emplace(pack, std::make_pair(ok ? fd : damaged ? -2 : -1, std::move(idx))).first;
    }
    if (it->second.first < 0) return it->second.first == -2 ? -3 : -2;   // -2 = no such pack, -3 = damaged / another version
    std::vector<uint8_t> v; const int r = pack_get(it->second.first, it->second.second, key, v);
    if (r <= 0) return r;   // 0 = not listed, -1 = unreadable / CRC differs
    uint8_t *o = static_cast<uint8_t *>(malloc(v.size() + 1)); if (!o) return -1;
    memcpy(o, v.data(), v.size()); *out = o; *n = v.size(); return 1;
}
#endif
