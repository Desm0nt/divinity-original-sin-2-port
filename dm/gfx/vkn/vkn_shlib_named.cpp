// [DOS2] VK-NATIVE stage 5.1 piece A1: engine shaders by name + Dref in the backend's shader library (vkn_shlib_named.h: the design,
// switches and contract). Ported from xbg_nr_sh.inc (namespace nrsh: on / dref_device / open_pack / get / apply / marked) and
// xbg_nr_sh_pass.inc (nrpass::choose); the table, the head / interface / descriptor checks and the pass check are dm/gfx/nr/nr_sh.c as
// it is. What differs from the old path: the switches' names (DM_VK_NATIVE_SH / _PCF), no DM_HACK_FSR / DM_HACK_LIGHTS swaps (not this
// piece), a pack without the 'd' bundles falls back to 'm' (the old path kept the translated one), and the translated module for a pass
// outside the verified one is a library shader of its own key ("<key>#nrt") instead of a second module on the translator's object.
#ifdef DM_GAME_DOS2
#include "vkn_shlib_named.h"
#include "vkn_shlib.h"
#include "vkn.h"
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
#include <mutex>
#include "../nr/nr_sh.h"
#include "../nr/nr_hacks.h"
#include "../vulkan/engine/xbg_vendor_policy.h"

// vkn_tex.c (weak: the shader-library host test links without it): the comparison twins made so far
extern "C" void vkn_sampler_cmp_stats(uint64_t *made, uint64_t *game_cmp, uint64_t *failed, uint32_t *alive) __attribute__((weak));

#define NS_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
namespace {
enum { ENTRIES = 16 };   // nr_sh.c NR_SH_MAX
struct Ent { char key[32]; uint64_t off; uint32_t size, pad; uint64_t hash; };   // xgspack index entry (xgs_pack.py)
static_assert(sizeof(Ent) == 56, "xgspack index entry");

int g_mode = -1, g_pcf = 0;                      // written once under g_init (call_once), read after it
std::once_flag g_init, g_pack;
std::string g_dir;
int g_fd = -1; std::vector<Ent> g_idx;
thread_local int t_bypass = 0;                   // > 0: reads inside this module (translated bytes wanted)
std::atomic<int> g_st[ENTRIES];                  // per entry: 0 not read yet, 1 replaced (mode 1) / passing every check (mode 2), -1 not
std::atomic<uint64_t> g_dref[ENTRIES];           // per entry: nr_sh_depth_bindings of the replaced module
std::atomic<uint64_t> c_swapped{0}, c_would{0}, c_kept{0}, c_head{0}, c_io{0}, c_desc{0}, c_missing{0}, c_off{0}, c_d2m{0}, c_dmiss{0},
    c_dref_sh{0}, c_flagged{0}, c_verified{0}, c_outside{0}, c_unchecked{0}, c_dref_kept{0}, c_alt{0}, c_alt_fail{0}, c_reds{0};
std::mutex g_pass_mu;
struct Seen { int entry; uint32_t hash; unsigned why; };
std::vector<Seen> g_seen;                        // under g_pass_mu

void L(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void L(const char *fmt, ...) { char b[768]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); vkn_logf("shlib named: %s", b); }
void red(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void red(const char *fmt, ...) {
    if (c_reds.fetch_add(1) >= 16) return;
    char b[768]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); vkn_logf(NS_RED "shlib named: %s", b);
}
const char *pcf_name(int q) { return q == 1 ? "exact" : q == 2 ? "mobile" : q == 3 ? "dref" : "translated"; }

// xbg_nr_sh.inc dref_device: the shadow atlas is NVN DEPTH16 (0x32, NR-SHCACHE 11.3), which vkn_tex.c stores as D16_UNORM (its depth
// stand-ins are for the D24 formats only). Linear filtering of a depth format is optional in Vulkan (Adreno 730 512.615 has none on
// D16); depth comparison of a sampled depth format is reported with Vulkan 1.3 / VK_KHR_format_feature_flags2 (checked when reported).
bool dref_device(VkPhysicalDevice phys, bool ffl2) {
    const VkFormat f = VK_FORMAT_D16_UNORM;
    VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(phys, f, &fp); const VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    bool cmp = true;
    if (ffl2) {
        VkFormatProperties3 p3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3}; VkFormatProperties2 p2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2}; p2.pNext = &p3;
        vkGetPhysicalDeviceFormatProperties2(phys, f, &p2); cmp = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
    }
    const bool sampled = (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0, linear = (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0, ok = sampled && linear && cmp;
    L("DM_VK_NATIVE_SH_PCF=3 (dref): shadow atlas (NVN DEPTH16) stored as D16_UNORM, features %#x (sampled %d, linear filter %d), depth comparison %s: %s",
      unsigned(ff), int(sampled), int(linear), ffl2 ? (cmp ? "yes" : "NO") : "not reported (pre-1.3: allowed)",
      ok ? "hardware compare used" : "NOT usable - DM_VK_NATIVE_SH_PCF=2 (mobile, textureGather) used instead");
    return ok;
}
void init(VkPhysicalDevice phys, bool ffl2, int policy) {
    const XbgGpuFamily fam = XbgGpuFamily(policy);
    const char *e = getenv("DM_VK_NATIVE_SH"); int m = e && *e ? atoi(e) : xbg_nr_sh_default(fam);
    if (m < 0 || m > 2) { red("DM_VK_NATIVE_SH=%s is not 0 / 1 / 2: off", e); m = 0; }
    const char *p = getenv("DM_VK_NATIVE_SH_PCF"); int q = p && *p ? atoi(p) : xbg_nr_sh_pcf_default(fam);
    if (q < 0 || q > 3) { red("DM_VK_NATIVE_SH_PCF=%s is not 0..3: translated shadows", p); q = 0; }
    {   // [DOS2] N6 DM_HACK_SHPCF (2 mobile / 3 dref; also from DM_HACK_TIER) when DM_VK_NATIVE_SH_PCF is not set (xbg_nr_sh.inc on)
        char hf = 0; const int hq = nrh_value(NRH_SHPCF, &hf);
        if (hq && !(p && *p)) { q = hq; L("DM_HACK_SHPCF=%d%s: shadow resolve %s (as DM_VK_NATIVE_SH_PCF=%d)", hq, hf == 't' ? " (tier)" : "", pcf_name(hq), hq); }
    }
    if (m && q == NR_SH_PCF_DREF && phys && !dref_device(phys, ffl2)) { q = NR_SH_PCF_MOBILE; c_d2m++; }
    g_pcf = q; g_mode = m;
    if (m) L("DM_VK_NATIVE_SH=%d (%s; replaces DM_NR_SH): engine shaders by name, %d in the table (shadows: DM_VK_NATIVE_SH_PCF=%d %s, policy family %s); "
             "source <shaders>/native-nr.xgspack (setup stage 8)", m, m == 1 ? "replace" : "verify: translated used", nr_sh_count, q, pcf_name(q), xbg_gpu_family_name(fam));
    else L("DM_VK_NATIVE_SH=0: engine shaders by name off (the translated bundles)");
}
// xbg_nr_sh.inc open_pack / get (the index checks of the shader library's pack reader; CRC-32 = zlib's)
void open_pack() {
    std::call_once(g_pack, [] {
        if (g_dir.empty()) return;
        const std::string path = g_dir + "/native-nr.xgspack"; const int f = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (f < 0) { red("no %s: run setup again (stage 8 builds it from your shader pack); translated shaders used", path.c_str()); return; }
        struct stat s{}; uint32_t h[4] = {};
        bool ok = !fstat(f, &s) && pread(f, h, 16, 0) == 16 && h[0] == 0x50534758u && h[1] == 1u && h[2] < 4096;
        std::vector<Ent> v; if (ok) { v.resize(h[2]); ok = pread(f, v.data(), v.size() * sizeof(Ent), 16) == ssize_t(v.size() * sizeof(Ent)); }
        for (size_t i = 0; ok && i < v.size(); i++) ok = v[i].key[31] == 0 && v[i].off >= 16 + v.size() * sizeof(Ent) && v[i].off + v[i].size <= uint64_t(s.st_size) && (!i || strcmp(v[i - 1].key, v[i].key) < 0);
        if (!ok) { ::close(f); red("%s is damaged or of another version: ignored (run setup again)", path.c_str()); return; }
        g_fd = f; g_idx.swap(v); L("%s: %zu bundles", path.c_str(), g_idx.size());
    });
}
bool get(const char *key, std::vector<uint8_t> &out) {
    open_pack(); if (g_fd < 0) return false;
    auto it = std::lower_bound(g_idx.begin(), g_idx.end(), key, [](const Ent &a, const char *k) { return strcmp(a.key, k) < 0; });
    if (it == g_idx.end() || strcmp(key, it->key)) return false;
    out.resize(it->size); size_t got = 0;
    while (got < out.size()) { const ssize_t r = pread(g_fd, out.data() + got, out.size() - got, off_t(it->off + got)); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; got += size_t(r); }
    if (got != out.size()) return false;
    uLong c = crc32(0L, Z_NULL, 0); c = crc32(c, out.data(), uInt(out.size()));
    if (uint64_t(uint32_t(c)) != it->hash) { red("native-nr.xgspack: bundle %s: CRC differs (damaged pack: run setup again)", key); return false; }
    return true;
}
std::vector<uint32_t> spirv_of(const std::vector<uint8_t> &b) {   // the bundle's SPIR-V as words (the head is not 4-byte aligned in general)
    const size_t h = nr_sh_head(b.data(), b.size()); std::vector<uint32_t> w; if (!h) return w;
    w.resize((b.size() - h) / 4); memcpy(w.data(), b.data() + h, w.size() * 4); return w;
}
void io(const std::vector<uint8_t> &b, uint32_t &in, uint32_t &out) {
    in = out = 0; const std::vector<uint32_t> w = spirv_of(b); if (w.empty()) return;
    if (nr_sh_io(w.data(), w.size(), &in, &out)) in = out = ~0u;
}
int entry(const std::string &key) { const int i = key.size() == 16 ? nr_sh_find_key(key.c_str()) : -1; return i >= 0 && i < ENTRIES ? i : -1; }
// xbg_nr_sh.inc apply: with the translated bundle in hand, mode 1 may swap in the named one (true), mode 2 checks it.
bool apply(int i, const std::string &key, std::vector<uint8_t> &bytes) {
    const nr_sh_entry &e = nr_sh_table[i]; char v = nr_sh_variant(i, g_pcf); int was = 0;
    if (!v) { if (g_st[i].compare_exchange_strong(was, -1)) { c_kept++; L("%s (%s): translated kept (DM_VK_NATIVE_SH_PCF=0; 1 exact, 2 mobile, 3 dref)", e.name, key.c_str()); } return false; }
    if (!nr_sh_enabled(i)) { if (g_st[i].exchange(-1) != -1) { c_off++; red("%s (%s): off - the engine named another shader with this key or name: translated used", e.name, key.c_str()); } return false; }
    char pk[32]; if (!nr_sh_pack_key(i, v, pk, sizeof pk)) return false;
    std::vector<uint8_t> nb; bool have = get(pk, nb);
    if (!have && v == 'd') {   // a pack built before the Dref bundles: the gather variant (the same shadow class, no compare sampler)
        char pm[32];
        if (nr_sh_variant(i, NR_SH_PCF_MOBILE) == 'm' && nr_sh_pack_key(i, 'm', pm, sizeof pm) && get(pm, nb)) {
            have = true; v = 'm'; memcpy(pk, pm, sizeof pk);
            if (g_st[i].load() == 0) { c_dmiss++; red("%s: no Dref bundle d.* in native-nr.xgspack (a pack from before PERF-X19 T11: run setup again): %s used", e.name, pk); }
        }
    }
    if (!have) { if (g_st[i].compare_exchange_strong(was, -1)) { c_missing++; if (g_fd >= 0) red("%s: no bundle %s in native-nr.xgspack (run setup again): translated used", e.name, pk); } return false; }
    const bool head = nr_sh_head_equal(bytes.data(), bytes.size(), nb.data(), nb.size()) != 0, binds = nr_sh_bindings_ok(nb.data(), nb.size()) != 0;
    uint32_t ti, to, ni, no; io(bytes, ti, to); io(nb, ni, no);
    if (!head || !binds || ti != ni || to != no) {
        if (g_st[i].exchange(-1) != -1) {
            (!head ? c_head : ti != ni || to != no ? c_io : c_desc)++;
            red("%s: %s refused - head %s (translated %016llx, pack %016llx), inputs %#x/%#x, outputs %#x/%#x (translated/pack), descriptors %s: "
                "translated used (the pack is not from this shader pack: run setup again)", e.name, pk, head ? "equal" : "DIFFERS",
                (unsigned long long)nr_sh_head_hash(bytes.data(), bytes.size()), (unsigned long long)nr_sh_head_hash(nb.data(), nb.size()), ti, ni, to, no,
                binds ? "in the table" : "NOT in the binding table");
        }
        return false;
    }
    uint64_t dmask = 0;
    if (g_mode == 1) {
        const std::vector<uint32_t> w = spirv_of(nb); dmask = nr_sh_depth_bindings(w.data(), w.size());
        if ((v == 'd') != (dmask != 0) && g_st[i].load() == 0)
            red("%s: %s declares %s depth-compare binding (mask %#llx): %s", e.name, pk, dmask ? "a" : "no", (unsigned long long)dmask,
                v == 'd' ? "a Dref bundle without one - sampled without the comparison sampler" : "a non-Dref bundle with one - the comparison sampler is set there too");
    }
    const size_t th = nr_sh_head(bytes.data(), bytes.size());
    if (g_st[i].compare_exchange_strong(was, 1)) {
        g_dref[i].store(dmask); if (dmask) c_dref_sh++;
        L("%s %s %s (%s%s): head byte for byte (%016llx), inputs %#x outputs %#x, SPIR-V %zu -> %zu words%s", e.name, g_mode == 1 ? "->" : "check:", pk,
          e.cls == NR_SH_SHADOW ? (v == 'x' ? "exact" : v == 'd' ? "dref: comparison sampler on its depth binding" : "mobile") : "proven bit for bit",
          e.proven ? "" : ", no interpreter proof", (unsigned long long)nr_sh_head_hash(bytes.data(), bytes.size()), ti, to, (bytes.size() - th) / 4, (nb.size() - th) / 4,
          g_mode == 1 ? "" : " - translated used (DM_VK_NATIVE_SH=2)");
    }
    if (g_mode == 1) { bytes.swap(nb); c_swapped++; return true; }
    c_would++; return false;
}
}  // namespace

int vkn_shlib_named_read(const std::string &key, std::vector<uint8_t> &out, bool &from_pack, vkn_shlib_reader read, const std::string &dir,
                         VkPhysicalDevice phys, bool ffl2, int policy) {
    if (t_bypass || !read) return -1;
    std::call_once(g_init, [&] { g_dir = dir; init(phys, ffl2, policy); });
    if (g_mode <= 0) return -1;
    // "<key>#nrt": the translated bundle of a replaced key (vkn_shlib_named_alt), read past this module
    if (key.size() == 20 && !key.compare(16, 4, "#nrt")) { t_bypass++; const bool have = read(key.substr(0, 16), out, from_pack); t_bypass--; return have ? 1 : 0; }
    const int i = entry(key); if (i < 0) return -1;
    t_bypass++; const bool have = read(key, out, from_pack); t_bypass--;
    if (have && !out.empty()) apply(i, key, out);
    return have ? 1 : 0;
}

extern "C" int vkn_shlib_named_entry(const char *fs_key, uint64_t *dref_mask) {
    if (dref_mask) *dref_mask = 0;
    if (g_mode <= 0 || !fs_key) return -1;
    const int i = entry(fs_key); if (i < 0 || g_st[i].load() != 1) return -1;
    if (dref_mask && g_mode == 1) *dref_mask = g_dref[i].load();
    return i;
}
extern "C" int vkn_shlib_named_pass(int i, int ncolor, const uint32_t *fmt, uint32_t depth_fmt, uint32_t samples, uint32_t w, uint32_t h) {
    if (i < 0 || i >= ENTRIES || i >= nr_sh_count) return 0;
    if (ncolor < 0) { c_unchecked++; return 0; }
    const uint32_t hash = nr_sh_pass_hash(ncolor, fmt, depth_fmt, samples); const unsigned why = nr_sh_pass_check(i, ncolor, fmt, samples);
    const nr_sh_entry &e = nr_sh_table[i]; const bool dref = g_mode == 1 && g_dref[i].load() != 0;
    bool fresh = true;
    { std::lock_guard<std::mutex> l(g_pass_mu); for (const Seen &x : g_seen) if (x.entry == i && x.hash == hash) { fresh = false; break; } if (fresh && g_seen.size() < 256) g_seen.push_back({i, hash, why}); else fresh = false; }
    if (fresh) {
        char cs[96]; int k = 0; cs[0] = 0;
        for (int c = 0; c < ncolor && c < 8 && k < int(sizeof cs) - 12; c++) k += snprintf(cs + k, sizeof cs - size_t(k), "%s%#x", c ? "," : "", fmt ? fmt[c] : 0u);
        if (!ncolor) snprintf(cs, sizeof cs, "none");
        if (!why) L("%s pass %08x: colours [%s] depth %#x samples %u, %ux%u - the verified pass", e.name, hash, cs, depth_fmt, samples, w, h);
        else red("%s pass %08x: colours [%s] depth %#x samples %u - not the verified pass (%d colour%s, colour 0 %#x, 1 sample; differs:%s%s%s): %s", e.name, hash, cs,
                 depth_fmt, samples, e.colors, e.colors == 1 ? "" : "s", unsigned(e.color0), why & NR_SH_P_COLORS ? " count" : "", why & NR_SH_P_FORMAT ? " format" : "",
                 why & NR_SH_P_SAMPLES ? " samples" : "", g_mode != 1 ? "counted (DM_VK_NATIVE_SH=2)" : dref ? "a Dref bundle keeps its module (comparison sampler)" : "the translated module is used in this pass");
    }
    if (!why) { c_verified++; return 0; }
    c_outside++;
    if (g_mode != 1) return 0;
    // T11: the comparison sampler is bound for every pipeline of the shader (the binding's flag); the translated module samples the
    // binding without Dref, which a comparison sampler makes undefined - the named module stays (it writes the same vec4 to location 0)
    if (dref) { c_dref_kept++; return 0; }
    return 1;
}
extern "C" int vkn_shlib_named_alt(const char *fs_key, xbg_vkn_stage *out) {
    if (!fs_key || strlen(fs_key) != 16 || !out) return -1;
    const std::string k = std::string(fs_key) + "#nrt";
    return vkn_shlib_shader(k.c_str(), nullptr, nullptr, out);
}
extern "C" void vkn_shlib_named_flagged(uint32_t n) { c_flagged += n; }
extern "C" void vkn_shlib_named_alt_result(int ok) {
    if (ok) { c_alt++; return; }
    c_alt_fail++; red("the translated module for a pass outside the verified one could not be made: the named module is used there");
}
extern "C" void vkn_shlib_named_stats_get(vkn_shlib_named_stats *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->mode = g_mode; s->pcf = g_pcf; s->pack_open = g_fd >= 0; s->pack_bundles = uint32_t(g_fd >= 0 ? g_idx.size() : 0);
    for (int i = 0; i < ENTRIES && i < nr_sh_count; i++) s->replaced += g_st[i].load() == 1;
    s->swapped = c_swapped; s->would = c_would; s->kept = c_kept; s->refused_head = c_head; s->refused_io = c_io; s->refused_desc = c_desc; s->missing = c_missing;
    s->off_name = c_off; s->dref_to_mobile = c_d2m; s->dref_missing = c_dmiss; s->dref_shaders = c_dref_sh; s->dref_flagged = c_flagged;
    s->pass_verified = c_verified; s->pass_outside = c_outside; s->pass_unchecked = c_unchecked; s->pass_dref_kept = c_dref_kept; s->alt_made = c_alt; s->alt_failed = c_alt_fail;
    { std::lock_guard<std::mutex> l(g_pass_mu); s->passes = uint32_t(g_seen.size()); }
    s->reds = c_reds;
}
extern "C" void vkn_shlib_named_census(void) {
    vkn_shlib_named_stats s; vkn_shlib_named_stats_get(&s);
    if (s.mode < 0) return;   // the library read no bundle yet (or is not the loader: modes 0 / 2, DM_VK_NATIVE_SHLIB=0)
    uint64_t tm = 0, tg = 0, tf = 0; uint32_t ta = 0; if (vkn_sampler_cmp_stats) vkn_sampler_cmp_stats(&tm, &tg, &tf, &ta);
    vkn_logf("shlib named: DM_VK_NATIVE_SH=%d (%s), DM_VK_NATIVE_SH_PCF=%d (%s): pack %s (%u bundles); entries %s %u of %d (bundle reads %s %llu), "
             "kept translated %llu, refused: head %llu / interface %llu / descriptors %llu, missing in the pack %llu, off by a name contradiction %llu; "
             "dref: to mobile by the atlas format %llu, 'd' missing -> 'm' %llu, shaders with a depth-compare binding %llu, bindings flagged %llu, comparison twins "
             "made %llu (game compare kept %llu, failed %llu, alive %u); pipelines in the verified pass %llu, outside it %llu (translated modules made %llu, failed %llu, "
             "Dref kept %llu), unchecked (built off the recording thread) %llu, distinct shader passes %u; red lines %llu",
             s.mode, s.mode == 1 ? "replace" : s.mode == 2 ? "verify" : "off", s.pcf, pcf_name(s.pcf), s.pack_open ? "open" : "none", s.pack_bundles,
             s.mode == 1 ? "replaced" : "passing every check", s.replaced, nr_sh_count, s.mode == 1 ? "swapped" : "checked",
             (unsigned long long)(s.mode == 1 ? s.swapped : s.would), (unsigned long long)s.kept, (unsigned long long)s.refused_head, (unsigned long long)s.refused_io,
             (unsigned long long)s.refused_desc, (unsigned long long)s.missing, (unsigned long long)s.off_name, (unsigned long long)s.dref_to_mobile,
             (unsigned long long)s.dref_missing, (unsigned long long)s.dref_shaders, (unsigned long long)s.dref_flagged, (unsigned long long)tm, (unsigned long long)tg,
             (unsigned long long)tf, ta, (unsigned long long)s.pass_verified, (unsigned long long)s.pass_outside, (unsigned long long)s.alt_made,
             (unsigned long long)s.alt_failed, (unsigned long long)s.pass_dref_kept, (unsigned long long)s.pass_unchecked, s.passes, (unsigned long long)s.reds);
}
#endif
