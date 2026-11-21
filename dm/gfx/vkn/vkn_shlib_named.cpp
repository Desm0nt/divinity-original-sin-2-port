// [DOS2] VK-NATIVE stage 5.1 piece A1: engine shaders by name + Dref in the backend's shader library (vkn_shlib_named.h: the design,
// switches and contract). Ported from xbg_nr_sh.inc (namespace nrsh: on / dref_device / open_pack / get / apply / marked) and
// xbg_nr_sh_pass.inc (nrpass::choose); the table, the head / interface / descriptor checks and the pass check are dm/gfx/nr/nr_sh.c as
// it is. What differs from the old path: the switches' names (DM_VK_NATIVE_SH / _PCF), a pack without the 'd' bundles falls back to
// 'm' (the old path kept the translated one), and the translated module for a pass outside the verified one is a library shader of
// its own key ("<key>#nrt") instead of a second module on the translator's object.
// [stage 5.4 G1] the DM_HACK_FSR / DM_HACK_LIGHTS module swaps of xbg_nr_sh.inc (hack_apply / hacks) are ported below (section
// "stage 5.4 G1"; the end block of vkn_shlib_named.h says how they compose with LIGHTFUSE and the fused post).
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
#include "../nr/nr_lightfuse.h"   // [stage 5.1 A4] nr_lf_bindings_ok (the fused bundle's extras)
#include "../vulkan/engine/xbg_vendor_policy.h"

// vkn_tex.c (weak: the shader-library host test links without it): the comparison twins made so far
extern "C" void vkn_sampler_cmp_stats(uint64_t *made, uint64_t *game_cmp, uint64_t *failed, uint32_t *alive) __attribute__((weak));
// [stage 5.2 B3] DM_VK_NATIVE_POST=0 keeps the post entries translated (vkn_post.h; weak: the library test links without vkn_post.c)
extern "C" int vkn_post_mobile(void) __attribute__((weak));
// [stage 5.4 G1] DM_VK_NATIVE_LCAP (vkn_hacks.c; weak: the library test links without it - then 1, the default)
extern "C" int vkn_hacks_lcap_mode(void) __attribute__((weak));
extern "C" int vkn_hacks_lcap_sel(void) __attribute__((weak));   // [gpu2] DM_VK_NATIVE_LCAP_SEL (weak: then the env rule of vkn_hacks.c)
extern "C" void vkn_hacks_census(void) __attribute__((weak));

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
// ---- [stage 5.4 G1] DM_HACK_FSR / DM_HACK_LIGHTS: the values (xbg_nr_sh.inc on), read with the library's other switches ----
int g_fsr = 0, g_lights = 0, g_lcap = 1, g_lsel = 1;   // written once under g_init, read after it (g_lsel: [gpu2] DM_VK_NATIVE_LCAP_SEL)
std::atomic<int> g_hk_st[2];                     // per hack (0 FSR, 1 light cap): 0 not seen, 1 swapped / checked, -1 refused
std::atomic<uint64_t> c_hk_swapped[2], c_hk_refused[2], c_hk_checked{0};
void hacks_init(XbgGpuFamily fam, int m) {
    char hf = 0; g_fsr = nrh_value(NRH_FSR, &hf);
    if (g_fsr && hf == 't' && !xbg_hack_fsr_tier_default(fam)) {
        g_fsr = 0; L("DM_HACK_TIER: FSR 1 EASU not used on the %s family (DM_HACK_FSR=1 forces it)", xbg_gpu_family_name(fam));
    }
    if (g_fsr) L("DM_HACK_FSR=%d%s: PPFXAA bundle -> %s (FXAA as before at a 1:1 stretch, FSR 1 EASU when the 3D is below 100 %%: "
                 "tools/dos2_post/PPFXAA_fsr.frag; RCAS = the gear's sharpening)%s", g_fsr, hf == 't' ? " (tier)" : "",
                 g_fsr == 1 ? "the FXAA + EASU half-precision module" : g_fsr == 3 ? "the FXAA + EASU fp32 module" : "check only",
                 m == 1 ? "" : m == 2 ? " - DM_VK_NATIVE_SH=2 (verify): not used" : " - DM_VK_NATIVE_SH=0: NOT used");
    char lf = 0; g_lights = nrh_value(NRH_LIGHTS, &lf);
    if (vkn_hacks_lcap_mode) g_lcap = vkn_hacks_lcap_mode();
    else { const char *e = getenv("DM_VK_NATIVE_LCAP"); g_lcap = e && *e ? atoi(e) : 2; if (g_lcap < 0 || g_lcap > 2) g_lcap = 2; }   // (vkn_hacks.c's rule)
    if (vkn_hacks_lcap_sel) g_lsel = vkn_hacks_lcap_sel();
    else {   // (vkn_hacks.c's rule, without its log lines)
        const char *se = getenv("DM_VK_NATIVE_LCAP_SEL"); g_lsel = se && *se ? atoi(se) : 1; if (g_lsel < 0 || g_lsel > 2) g_lsel = 1;
        if (g_lsel == 2 && g_lcap == 2) g_lcap = 1;
    }
    if (g_lights && !g_lcap) {
        red("DM_HACK_LIGHTS=%d%s set, DM_VK_NATIVE_LCAP=0: the light cap is NOT applied (all clustered lights)", g_lights, lf == 't' ? " (tier)" : "");
        g_lights = 0;
    } else if (g_lights)
        L("DM_HACK_LIGHTS=%d%s: ClusteredLightingWithSSAO -> %s (DM_VK_NATIVE_LCAP_SEL=%d); DM_VK_NATIVE_LCAP=%d%s", g_lights, lf == 't' ? " (tier)" : "",
          g_lsel == 0 ? (g_lights <= 2 ? "the first 2 clustered lights that light a pixel (points, then spots, list order)"
                                       : "the first 4 clustered lights that light a pixel (points, then spots, list order)")
                      : g_lights <= 2 ? "the clustered lights that add less than 1/16 of the pixel's own light skipped, faded up to 1/8"
                                      : "the clustered lights that add less than 1/64 of the pixel's own light skipped, faded up to 1/32", g_lsel,
          g_lcap, m == 1 ? (g_lcap == 1 ? " (LIGHTFUSE yields while the cap is in use: vkn_hacks.h)" : "") : m == 2 ? " - DM_VK_NATIVE_SH=2 (verify): not used" : " - DM_VK_NATIVE_SH=0: NOT used");
    if ((g_fsr || g_lights) && m != 1) red("DM_HACK_FSR=%d / DM_HACK_LIGHTS=%d need DM_VK_NATIVE_SH=1 (it is %d): not applied", g_fsr, g_lights, m);
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
    hacks_init(fam, m);   // [stage 5.4 G1] DM_HACK_FSR / DM_HACK_LIGHTS
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
// ---- [stage 5.4 G1] xbg_nr_sh.inc hack_apply / hacks: an entry's bundle -> a module built into the backend (no setup run) behind the
// translated bundle's own head - the same checks as a pack bundle (head byte for byte by construction, interface, descriptors);
// swapped = the entry is marked as replaced (the pass check of vkn_shlib_named_pass applies at pipeline creation, its "#nrt" twin is
// the translated module). A refusal keeps the usual path (the pack's named bundle, or the translated one).
//   DM_HACK_FSR    PPFXAA -> tools/dos2_post/PPFXAA_fsr.frag: its FXAA half = PPFXAA_mobile.frag (bit for bit), FSR 1 EASU when the 3D
//                  is below 100 % (stretch ratio = coordinate derivative x texture width). 1 = half precision (FsrEasuH form),
//                  3 = fp32 (FsrEasuF), 2 = checked only.
//   DM_HACK_LIGHTS ClusteredLightingWithSSAO -> ClusteredLightingWithSSAO_mobile.frag + LCAP 2 / 4 (tools/dos2_light_cap.py): the
//                  first N clustered lights that light the pixel, then the loops stop; = the 'x' bundle on a prefix of the list, bit for bit.
const uint32_t k_fsr_spv[] = {
#include "../../android/shaders/ppfxaa_fsr.frag.spv.inc"
};
const uint32_t k_fsrh_spv[] = {
#include "../../android/shaders/ppfxaa_fsrh.frag.spv.inc"
};
const uint32_t k_lcap2_spv[] = {
#include "../../android/shaders/light_lcap2.frag.spv.inc"
};
const uint32_t k_lcap4_spv[] = {
#include "../../android/shaders/light_lcap4.frag.spv.inc"
};
// DM_VK_NATIVE_LCAP=2 (vkn_hacks.h): the cap inside the fused Sky -> Light module ('f') and with the fog ('g'); tools/dos2_light_cap_fused.py
// (proof: = Sky -> q11 -> the capped Light -> blend, bit for bit). Compiled with the DeferredSky c1 bits k_lcf_c1 (their first line).
const uint32_t k_lcap2f_spv[] = {
#include "../../android/shaders/light_lcap2_f.frag.spv.inc"
};
const uint32_t k_lcap2g_spv[] = {
#include "../../android/shaders/light_lcap2_g.frag.spv.inc"
};
const uint32_t k_lcap4f_spv[] = {
#include "../../android/shaders/light_lcap4_f.frag.spv.inc"
};
const uint32_t k_lcap4g_spv[] = {
#include "../../android/shaders/light_lcap4_g.frag.spv.inc"
};
// [gpu2] DM_VK_NATIVE_LCAP_SEL=1 (default): the per-light threshold (tools/dos2_light_cap.py thr_source; fused: tools/dos2_light_cap_fused.py,
// the same Sky c1), and =2 its poison twin (the cap module only)
const uint32_t k_lcap2t_spv[] = {
#include "../../android/shaders/light_lcap2t.frag.spv.inc"
};
const uint32_t k_lcap4t_spv[] = {
#include "../../android/shaders/light_lcap4t.frag.spv.inc"
};
const uint32_t k_lcap2tp_spv[] = {
#include "../../android/shaders/light_lcap2tp.frag.spv.inc"
};
const uint32_t k_lcap4tp_spv[] = {
#include "../../android/shaders/light_lcap4tp.frag.spv.inc"
};
const uint32_t k_lcap2tf_spv[] = {
#include "../../android/shaders/light_lcap2t_f.frag.spv.inc"
};
const uint32_t k_lcap2tg_spv[] = {
#include "../../android/shaders/light_lcap2t_g.frag.spv.inc"
};
const uint32_t k_lcap4tf_spv[] = {
#include "../../android/shaders/light_lcap4t_f.frag.spv.inc"
};
const uint32_t k_lcap4tg_spv[] = {
#include "../../android/shaders/light_lcap4t_g.frag.spv.inc"
};
const uint32_t k_lcf_c1[2] = {0x40638bacu, 0x3db851ecu};   // SKY_C1 of light_lcap*_{f,g}.frag.spv.inc (the player's bundle: 3.5554, 0.09)
std::atomic<int> g_lcf{0};                                  // 0 not checked, 1 the player's Sky c1 = k_lcf_c1, -1 differs / unreadable
std::atomic<uint64_t> c_lcf{0};                             // capped fused bundles handed out
bool hack_apply(int i, int slot, const std::string &key, std::vector<uint8_t> &bytes, const uint32_t *spv, size_t spv_bytes, const char *what, bool use) {
    const size_t th = nr_sh_head(bytes.data(), bytes.size()); if (!th) return false;
    std::vector<uint8_t> nb(bytes.begin(), bytes.begin() + th); nb.resize(th + spv_bytes); memcpy(nb.data() + th, spv, spv_bytes);
    const uint32_t len = uint32_t(spv_bytes); memcpy(nb.data() + 24, &len, 4);   // the SPIR-V length word (h[6])
    uint32_t ti, to, ni, no; io(bytes, ti, to); io(nb, ni, no);
    const bool binds = nr_sh_bindings_ok(nb.data(), nb.size()) != 0, head = nr_sh_head_equal(bytes.data(), bytes.size(), nb.data(), nb.size()) != 0;
    if (!head || !binds || ti != ni || to != no) {
        c_hk_refused[slot]++;
        if (g_hk_st[slot].exchange(-1) != -1)
            red("%s (%s): %s refused - head %s, inputs %#x/%#x, outputs %#x/%#x, descriptors %s: the usual path", nr_sh_table[i].name, key.c_str(), what,
                head ? "equal" : "DIFFERS", ti, ni, to, no, binds ? "in the table" : "NOT in the binding table");
        return false;
    }
    int was = 0;
    if (g_hk_st[slot].compare_exchange_strong(was, 1))
        L("%s: %s (%s) %s (%zu -> %zu words), inputs %#x outputs %#x", what, nr_sh_table[i].name, key.c_str(), use ? "->" : "check only:", (bytes.size() - th) / 4, spv_bytes / 4, ti, to);
    if (!use) { c_hk_checked++; return false; }
    was = 0;
    if (g_st[i].compare_exchange_strong(was, 1)) g_dref[i].store(0);   // marked: the pass check at pipeline creation (no Dref binding)
    bytes = std::move(nb); c_hk_swapped[slot]++; c_swapped++; return true;
}
bool hacks(int i, const std::string &key, std::vector<uint8_t> &bytes) {
    if (g_mode != 1 || !nr_sh_enabled(i)) return false;
    const char *n = nr_sh_table[i].name;
    if (g_fsr && !strcmp(n, "PPFXAA"))
        return g_fsr == 3 ? hack_apply(i, 0, key, bytes, k_fsr_spv, sizeof k_fsr_spv, "DM_HACK_FSR=3 FXAA + EASU fp32 module", true)
                          : hack_apply(i, 0, key, bytes, k_fsrh_spv, sizeof k_fsrh_spv, "DM_HACK_FSR FXAA + EASU half module", g_fsr == 1);
    if (g_lights && !strcmp(n, "ClusteredLightingWithSSAO")) {
        if (g_lsel == 0)
            return g_lights <= 2 ? hack_apply(i, 1, key, bytes, k_lcap2_spv, sizeof k_lcap2_spv, "DM_HACK_LIGHTS=2 light cap module (first N)", true)
                                 : hack_apply(i, 1, key, bytes, k_lcap4_spv, sizeof k_lcap4_spv, "DM_HACK_LIGHTS=4 light cap module (first N)", true);
        if (g_lsel == 2)
            return g_lights <= 2 ? hack_apply(i, 1, key, bytes, k_lcap2tp_spv, sizeof k_lcap2tp_spv, "DM_HACK_LIGHTS=2 light cap module (weak lights, POISON)", true)
                                 : hack_apply(i, 1, key, bytes, k_lcap4tp_spv, sizeof k_lcap4tp_spv, "DM_HACK_LIGHTS=4 light cap module (weak lights, POISON)", true);
        return g_lights <= 2 ? hack_apply(i, 1, key, bytes, k_lcap2t_spv, sizeof k_lcap2t_spv, "DM_HACK_LIGHTS=2 light cap module (weak lights, tau 1/16)", true)
                             : hack_apply(i, 1, key, bytes, k_lcap4t_spv, sizeof k_lcap4t_spv, "DM_HACK_LIGHTS=4 light cap module (weak lights, tau 1/64)", true);
    }
    return false;
}
// xbg_nr_sh.inc apply: with the translated bundle in hand, mode 1 may swap in the named one (true), mode 2 checks it.
bool apply(int i, const std::string &key, std::vector<uint8_t> &bytes) {
    const nr_sh_entry &e = nr_sh_table[i]; char v = nr_sh_variant(i, g_pcf); int was = 0;
    if (!v) { if (g_st[i].compare_exchange_strong(was, -1)) { c_kept++; L("%s (%s): translated kept (DM_VK_NATIVE_SH_PCF=0; 1 exact, 2 mobile, 3 dref)", e.name, key.c_str()); } return false; }
    if (e.cls == NR_SH_POST && vkn_post_mobile && !vkn_post_mobile()) {   // [stage 5.2 B3] the Switch post shaders (DM_VK_NATIVE_POST=0)
        if (g_st[i].compare_exchange_strong(was, -1)) { c_kept++; L("%s (%s): translated kept (DM_VK_NATIVE_POST=0: the Switch post shaders)", e.name, key.c_str()); }
        return false;
    }
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
// ---- [stage 5.1 A4] LIGHTFUSE (vkn_shlib_named.h, dm/gfx/vkn/vkn_lightfuse.c): the fused bundle, checked as xbg_nr_sh.inc oc_apply ----
std::atomic<vkn_shlib_reader> g_read{nullptr};   // the library's reader (saved at its first bundle read)
std::atomic<uint64_t> c_lf_ok{0}, c_lf_refused{0};
// The fused bundle "<v>.ClusteredLightingWithSSAO" for the translated bundle `tr` of the Light key: 1 = `nb` holds it and it passes.
int lcf_ok();   // [stage 5.4 G1] below
int lf_bundle(int i, char v, const std::vector<uint8_t> &tr, std::vector<uint8_t> &nb, const char **why) {
    char pk[32]; *why = NULL;
    if (g_lights && g_lcap == 2) {   // [stage 5.4 G1] DM_VK_NATIVE_LCAP=2: the capped fused module (built in) behind the translated head
        if (!lcf_ok()) { *why = "DM_VK_NATIVE_LCAP=2: the player's DeferredSky c1 differs from the built-in capped modules' (the cap module draws, no fusion)"; return 0; }
        const bool t = g_lsel != 0;   // [gpu2] the threshold cap (SEL 1; SEL 2 never comes here: DM_VK_NATIVE_LCAP is 1 with it)
        const uint32_t *spv = g_lights <= 2 ? (v == 'g' ? (t ? k_lcap2tg_spv : k_lcap2g_spv) : (t ? k_lcap2tf_spv : k_lcap2f_spv))
                                            : (v == 'g' ? (t ? k_lcap4tg_spv : k_lcap4g_spv) : (t ? k_lcap4tf_spv : k_lcap4f_spv));
        const size_t sb = g_lights <= 2 ? (v == 'g' ? (t ? sizeof k_lcap2tg_spv : sizeof k_lcap2g_spv) : (t ? sizeof k_lcap2tf_spv : sizeof k_lcap2f_spv))
                                        : (v == 'g' ? (t ? sizeof k_lcap4tg_spv : sizeof k_lcap4g_spv) : (t ? sizeof k_lcap4tf_spv : sizeof k_lcap4f_spv));
        const size_t th = nr_sh_head(tr.data(), tr.size()); if (!th) { *why = "no translated head"; return 0; }
        nb.assign(tr.begin(), tr.begin() + th); nb.resize(th + sb); memcpy(nb.data() + th, spv, sb);
        const uint32_t len = uint32_t(sb); memcpy(nb.data() + 24, &len, 4);   // the SPIR-V length word (h[6])
        c_lcf++;
    } else {
    if (!nr_sh_pack_key(i, v, pk, sizeof pk)) { *why = "no pack key"; return 0; }
    if (!get(pk, nb)) { *why = g_fd < 0 ? "no native-nr.xgspack" : "no such bundle in native-nr.xgspack (run setup again)"; return 0; }
    }
    uint32_t ti, to, ni, no; io(tr, ti, to); io(nb, ni, no);
    if (!nr_sh_head_equal(tr.data(), tr.size(), nb.data(), nb.size())) *why = "head differs from the translated bundle";
    else if (!nr_lf_bindings_ok(nb.data(), nb.size())) *why = "descriptors outside the head table + the extras, or an extra missing";
    else if (ti != ni || ti == ~0u) *why = "input locations differ";
    else if (v == 'g' ? no != (to | 2u) : no != to) *why = "output locations differ";
    return *why ? 0 : 1;
}
int lf_light_entry(const std::string &key16) { const int i = entry(key16); return i >= 0 && i == nr_sh_find_name("ClusteredLightingWithSSAO") ? i : -1; }
// [stage 5.4 G1] DM_VK_NATIVE_LCAP=2: the player's DeferredSky bundle (read past this module) carries the c1 bits the capped fused
// modules were compiled with (tools/dos2_lightfuse.py sky_c1: XGS1 head h[8], c1 at 32 + h[3] * 32 + h[4] * 4 when h[5] >= 8). Once.
int lcf_ok() {
    const int st = g_lcf.load(); if (st) return st > 0;
    const vkn_shlib_reader read = g_read.load(); const int sky = nr_sh_find_name("DeferredSky");
    std::vector<uint8_t> b; bool fp = false; uint32_t c[2] = {0, 0}; bool ok = false, have = false;
    if (read && sky >= 0) { t_bypass++; have = read(std::string(nr_sh_table[sky].key, 16), b, fp); t_bypass--; }
    if (have && b.size() >= 32) {
        uint32_t h[8]; memcpy(h, b.data(), sizeof h); const size_t o = 32 + size_t(h[3]) * 32 + size_t(h[4]) * 4;
        if (h[5] >= 8 && o + 8 <= b.size()) { memcpy(c, b.data() + o, 8); ok = c[0] == k_lcf_c1[0] && c[1] == k_lcf_c1[1]; }
    }
    int was = 0;
    if (g_lcf.compare_exchange_strong(was, ok ? 1 : -1)) {
        if (ok) L("DM_VK_NATIVE_LCAP=2: DeferredSky c1 %08x %08x = the built-in capped fused modules': LIGHTFUSE draws with the cap (light_lcap%d%s_f / _g: %s)", c[0], c[1],
                  g_lights <= 2 ? 2 : 4, g_lsel ? "t" : "", g_lsel ? "the weak lights of each pixel skipped" : "the first N lights");
        else red("DM_VK_NATIVE_LCAP=2: DeferredSky bundle %s (c1 %08x %08x, built-in %08x %08x): no capped fusion - the cap module draws, LIGHTFUSE refuses its Light draws",
                 have ? "of another c1" : "not readable", c[0], c[1], k_lcf_c1[0], k_lcf_c1[1]);
    }
    return g_lcf.load() > 0;
}
// read_bundle's door for "<key>#nlf" / "<key>#nlg": the checked fused bundle in place of a file (from_pack: it comes from a pack)
int lf_read(const std::string &key, std::vector<uint8_t> &out, bool &from_pack, vkn_shlib_reader read) {
    const std::string k16 = key.substr(0, 16); const char v = key[19] == 'g' ? 'g' : 'f';
    const int i = lf_light_entry(k16); if (i < 0) return 0;
    std::vector<uint8_t> tr; bool fp = false;
    t_bypass++; const bool have = read(k16, tr, fp); t_bypass--;
    if (!have || tr.empty()) return 0;
    std::vector<uint8_t> nb; const char *why = NULL;
    if (!lf_bundle(i, v, tr, nb, &why)) return 0;   // (vkn_shlib_named_fused said why already)
    out.swap(nb); from_pack = true; return 1;
}
}  // namespace

int vkn_shlib_named_read(const std::string &key, std::vector<uint8_t> &out, bool &from_pack, vkn_shlib_reader read, const std::string &dir,
                         VkPhysicalDevice phys, bool ffl2, int policy) {
    if (t_bypass || !read) return -1;
    std::call_once(g_init, [&] { g_dir = dir; g_read.store(read); init(phys, ffl2, policy); });
    if (key.size() == 20 && (!key.compare(16, 4, "#nlf") || !key.compare(16, 4, "#nlg"))) return lf_read(key, out, from_pack, read);   // [stage 5.1 A4] LIGHTFUSE (any DM_VK_NATIVE_SH)
    if (g_mode <= 0) return -1;
    // "<key>#nrt": the translated bundle of a replaced key (vkn_shlib_named_alt), read past this module
    if (key.size() == 20 && !key.compare(16, 4, "#nrt")) { t_bypass++; const bool have = read(key.substr(0, 16), out, from_pack); t_bypass--; return have ? 1 : 0; }
    const int i = entry(key); if (i < 0) return -1;
    t_bypass++; const bool have = read(key, out, from_pack); t_bypass--;
    if (have && !out.empty() && !((g_fsr || g_lights) && hacks(i, key, out))) apply(i, key, out);   // [stage 5.4 G1] the hacks first (xbg_nr_sh.inc apply)
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
extern "C" void vkn_shlib_named_hacks_get(vkn_shlib_named_hacks *h) {   // [stage 5.4 G1]
    if (!h) return;
    memset(h, 0, sizeof *h);
    h->read = g_mode >= 0; h->sh_mode = g_mode; h->fsr = g_fsr; h->lights = g_lights; h->lcap = g_lcap; h->lsel = g_lsel; h->checked = c_hk_checked;
    h->lcf = g_lcf.load(); h->lcf_bundles = c_lcf;
    for (int k = 0; k < 2; k++) { h->state[k] = g_hk_st[k].load(); h->swapped[k] = c_hk_swapped[k]; h->refused[k] = c_hk_refused[k]; }
}
extern "C" void vkn_shlib_named_census(void) {
    if (vkn_hacks_census) vkn_hacks_census();   // [stage 5.4 G3] the DM_HACK_* lines ride on this census (dm_vkn_census has 16 slots)
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
// ---- [stage 5.1 A4] LIGHTFUSE doors (vkn_shlib_named.h) ----
extern "C" int vkn_shlib_named_fused(const char *light_fs_key, char variant, xbg_vkn_stage *out, uint64_t declared[4], uint64_t *dref) {
    if (declared) memset(declared, 0, 4 * sizeof declared[0]);
    if (dref) *dref = 0;
    const vkn_shlib_reader read = g_read.load();
    if (!light_fs_key || strlen(light_fs_key) != 16 || !out || (variant != 'f' && variant != 'g')) return 1;
    if (!read || !vkn_shlib_ready()) return -1;
    const std::string k16(light_fs_key); const int i = lf_light_entry(k16);
    if (i < 0) { c_lf_refused++; red("lightfuse: %s is not the fragment key of ClusteredLightingWithSSAO: no fused bundle", light_fs_key); return 1; }
    std::vector<uint8_t> tr, nb; bool fp = false; const char *why = NULL;
    t_bypass++; const bool have = read(k16, tr, fp); t_bypass--;
    if (!have || tr.empty()) why = "the translated bundle of the key is missing";
    else lf_bundle(i, variant, tr, nb, &why);
    if (why) { c_lf_refused++; red("lightfuse: %c.ClusteredLightingWithSSAO for %s refused: %s (no fusion)", variant, light_fs_key, why); return 1; }
    const std::vector<uint32_t> w = spirv_of(nb);
    for (size_t k = 5; k < w.size();) {   // OpDecorate (71) <id> Binding (33) <n>
        const uint32_t c = w[k] >> 16, op = w[k] & 0xffff;
        if (!c || k + c > w.size()) break;
        if (op == 71 && c >= 4 && w[k + 2] == 33 && w[k + 3] < 256 && declared) declared[w[k + 3] >> 6] |= 1ull << (w[k + 3] & 63);
        k += c;
    }
    if (dref) *dref = nr_sh_depth_bindings(w.data(), w.size());
    const std::string lk = k16 + (variant == 'g' ? "#nlg" : "#nlf");
    const int rc = vkn_shlib_shader(lk.c_str(), nullptr, nullptr, out);
    if (rc || !out->module || out->stage != 4) {
        c_lf_refused++; red("lightfuse: %c.ClusteredLightingWithSSAO for %s: the library made no fragment module (rc %d, stage %u): no fusion", variant, light_fs_key, rc, rc ? 0u : out->stage);
        return rc < 0 ? -1 : 1;
    }
    c_lf_ok++;
    L("lightfuse: %c.ClusteredLightingWithSSAO for %s: %zu words, %u bindings in the head table (DeferredSky inside%s; extras: Sky's 8 textures on 200..242, c8 / c6 on 248 / 254)",
      variant, light_fs_key, w.size(), out->binding_count, variant == 'g' ? ", DeferredFog on output 1" : "");
    return 0;
}
extern "C" int vkn_shlib_named_same_bundle(const char *key_a, const char *key_b) {
    const vkn_shlib_reader read = g_read.load();
    if (!read) return -1;
    if (!key_a || !key_b) return 0;
    if (!strcmp(key_a, key_b)) return 1;
    std::vector<uint8_t> a, b; bool fp = false;
    t_bypass++; const bool ha = read(key_a, a, fp), hb = read(key_b, b, fp); t_bypass--;
    return ha && hb && !a.empty() && a == b ? 1 : 0;
}
#endif
