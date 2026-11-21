// [DOS2] VK-NATIVE stage 5.3, piece V2: driver corrections of the backend (vkn_drv.h: the table of corrections, switches, defaults and
// call sites). The rules are the old path's (xbg_vendor_policy.h, in C in vkn_policy.h); this file resolves them once for the device,
// prints them, owns the creation lock's fallback mutex and the counters of the census line.
#ifdef DM_GAME_DOS2
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn.h"
#include "vkn_drv.h"
#include "vkn_policy.h"
#include "vkn_shader.h"

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

// The shader library (vkn_shlib.cpp) and the draw path (vkn_draw.c): weak, so host tests and builds without them link.
extern int vkn_shlib_ready(void) __attribute__((weak));
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_policy(void) __attribute__((weak));
extern int vkn_shlib_serial(void) __attribute__((weak));
extern int vkn_shlib_fs_noout(const void *shader) __attribute__((weak));
extern void vkn_shlib_create_lock(void) __attribute__((weak));
extern void vkn_shlib_create_unlock(void) __attribute__((weak));
extern void vkn_draw_raw_begin(void) __attribute__((weak));
// Vulkan 1.2 core (vk_tramp_names.h lists it); weak: the host tests' mock has no such entry point (driverID 0 there).
extern VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice, VkPhysicalDeviceProperties2 *) __attribute__((weak));

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;     // resolve
static pthread_mutex_t g_own = PTHREAD_MUTEX_INITIALIZER;    // the creation lock without the shader library
static vkn_drv_cfg g_cfg;                                    // published by g_ready (release / acquire)
static int g_ready;
static const vkn_drv_cfg k_none = {.resolved = 0, .family = VKN_FAM_OTHER, .ubo_dyn = 1};   // before a device: nothing changes
static vkn_drv_stats g_s;   // atomics (build / clear / shader-creation time only)
#define ADD(f, n) __atomic_add_fetch(&g_s.f, (uint64_t)(n), __ATOMIC_RELAXED)

void vkn_drv_decide(int family, uint32_t driver_id, const char *sim, vkn_drv_cfg *o) {
    memset(o, 0, sizeof *o);
    o->resolved = 1; o->family = family; o->driver_id = driver_id;
    int from = 0;
    o->noout = (uint8_t)vkn_policy_env_or("DM_GFX_NOOUT_MASK", vkn_policy_noout_mask_default(family), &from); if (from) o->from_env |= VKN_DRV_E_NOOUT;
    o->bias_x2 = (uint8_t)vkn_policy_env_or("DM_GFX_DEPTH_BIAS_X2", vkn_policy_depth_bias_x2_default(driver_id), &from); if (from) o->from_env |= VKN_DRV_E_BIAS;
    o->rebind = (uint8_t)vkn_policy_env_or("DM_GFX_CLEAR_REBIND", vkn_policy_clear_rebind_default(driver_id), &from); if (from) o->from_env |= VKN_DRV_E_REBIND;
    o->serial = (uint8_t)vkn_policy_env_or("DM_GFX_PIPE_SERIAL", vkn_policy_pipe_serial_default(family), &from); if (from) o->from_env |= VKN_DRV_E_SERIAL;
    // NR-08b as nrubd::init (xbg_nr_ubodyn.inc) / the shader library's init: DM_NR_UBODYN 3 does not exist (0), out of range 0
    const char *e = getenv("DM_NR_UBODYN"); int m = e && *e ? atoi(e) : -1;
    if (m == 3) m = 0;
    if (m < -1 || m > 3) m = 0;
    if (e && *e) o->from_env |= VKN_DRV_E_UBODYN;
    const char *u = getenv("DM_GFX_UBO_DYNAMIC"); const int uenv = u && *u ? atoi(u) : -1;
    const int fam_on = vkn_policy_ubo_dynamic_default(family);
    o->ubo_ro = (uint8_t)vkn_policy_ubodyn_active(m, uenv, fam_on, family);
    int mode = m < 0 ? vkn_policy_ubodyn_default(family) : m; if (o->ubo_ro && mode != 1) mode = 1;   // DM_GFX_UBO_DYNAMIC=2 forces the rule
    o->ubodyn = (int8_t)mode;
    o->ubo_dyn = (uint8_t)vkn_policy_env_or("DM_GFX_UBO_DYNAMIC", fam_on, &from); if (from) o->from_env |= VKN_DRV_E_UBO;
    if (o->ubo_ro) o->ubo_dyn = 1;   // (nrubd::init: ubo_dynamic_on = true under the rule)
    o->fb_exact = (uint8_t)vkn_policy_env_or("DM_VK_NATIVE_FB_EXACT", vkn_policy_fb_exact_default(driver_id), &from); if (from) o->from_env |= VKN_DRV_E_FBX;   // [VK-DRV]
    o->sim_noaniso = (uint8_t)vkn_policy_sim_noaniso(sim);
    o->sim_ubdyn8 = (uint8_t)vkn_policy_sim_ubdyn8(sim);
}

static void install_line(const vkn_drv_cfg *c) {
    const char *fam = vkn_policy_family_name(c->family);
#define SRC(bit) ((c->from_env & (bit)) ? "env" : "default")
    vkn_logf("driver corrections (family %s, driverID %u%s): DM_GFX_NOOUT_MASK=%d (%s), DM_GFX_DEPTH_BIAS_X2=%d (%s), DM_GFX_CLEAR_REBIND=%d (%s), "
             "DM_GFX_PIPE_SERIAL=%d (%s; lock of the %s), DM_GFX_UBO_DYNAMIC=%d (%s), DM_NR_UBODYN=%d (%s; read-only rule %s); SIM ubdyn8 %d noaniso %d; "
             "reversed depth range %s (not emulated); DM_VK_NATIVE_FB_EXACT=%d (%s); census line \"driver corrections:\"",
             fam, c->driver_id, c->driver_id ? "" : " = unknown: no driver correction", c->noout, SRC(VKN_DRV_E_NOOUT), c->bias_x2, SRC(VKN_DRV_E_BIAS),
             c->rebind, SRC(VKN_DRV_E_REBIND), c->serial, SRC(VKN_DRV_E_SERIAL), c->lock_lib ? "shader library (modules + pipelines)" : "backend (pipelines only)",
             c->ubo_dyn, SRC(VKN_DRV_E_UBO), c->ubodyn, SRC(VKN_DRV_E_UBODYN), c->ubo_ro ? "in force: per-draw blocks dynamic only where read" : "off",
             c->sim_ubdyn8, c->sim_noaniso, vkn_policy_reversed_depth_broken(c->driver_id) ? "drawn wrong by this driver" : "fine", c->fb_exact, SRC(VKN_DRV_E_FBX));
#undef SRC
}

static void resolve(void) {
    pthread_mutex_lock(&g_mu);
    const vkn_dev *d = vkn_device();
    if (!g_ready && d) {
        const char *sim = getenv("DM_GFX_SIM_GPU");
        const int lib = vkn_shlib_ready && vkn_shlib_ready() && vkn_shlib_create_lock && vkn_shlib_create_unlock;
        const int f0 = vkn_shlib_policy ? vkn_shlib_policy() : -1;
        const int fam = f0 >= 0 ? f0 : vkn_policy_family_from(d->props.vendorID, d->props.deviceName, sim);
        uint32_t drv = 0;
        if (d->props.apiVersion >= VK_API_VERSION_1_2 && vkGetPhysicalDeviceProperties2 && d->phys) {
            VkPhysicalDeviceDriverProperties dp; memset(&dp, 0, sizeof dp); dp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 p2; memset(&p2, 0, sizeof p2); p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; p2.pNext = &dp;
            vkGetPhysicalDeviceProperties2(d->phys, &p2); drv = (uint32_t)dp.driverID;
        }
        vkn_drv_cfg c; vkn_drv_decide(fam, drv, sim, &c);
        c.lock_lib = (uint8_t)lib;
        if (lib && vkn_shlib_serial) {   // the library decided its module lock from the same switch and family: one answer (else a red line)
            const int ls = vkn_shlib_serial() != 0;
            if (ls != c.serial) { vkn_logf(RED "driver corrections: DM_GFX_PIPE_SERIAL: the shader library says %d, the rule here %d: the library's answer is used", ls, c.serial); ADD(reds, 1); c.serial = (uint8_t)ls; }
        }
        g_cfg = c;
        __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
        install_line(&g_cfg);
    }
    pthread_mutex_unlock(&g_mu);
}
const vkn_drv_cfg *vkn_drv(void) {
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return &g_cfg;
    resolve();
    return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) ? &g_cfg : &k_none;
}

int vkn_drv_lock(void) {
    const vkn_drv_cfg *c = vkn_drv();
    if (!c->serial) return 0;
    if (c->lock_lib) vkn_shlib_create_lock(); else pthread_mutex_lock(&g_own);
    ADD(locked, 1);
    return 1;
}
void vkn_drv_unlock(int took) {
    if (!took) return;
    if (g_cfg.lock_lib) vkn_shlib_create_unlock(); else pthread_mutex_unlock(&g_own);   // (took: g_cfg was resolved by vkn_drv_lock)
}

// The fact "fragment stage without colour output" of an engine shader: the shader library's (1 / 0 / -1 unknown), or a host test's stand-in.
static int lib_noout(const vkn_shader *s) {
    if (!(vkn_shlib_on && vkn_shlib_on() && vkn_shlib_fs_noout) || !s->xs[VKN_SRC_FS]) return -1;
    return vkn_shlib_fs_noout(s->xs[VKN_SRC_FS]);
}
static int (*g_facts)(const vkn_shader *s) = lib_noout;
void vkn_drv_test_facts(int (*fn)(const vkn_shader *s)) { g_facts = fn ? fn : lib_noout; }
int vkn_drv_noout(const vkn_pipe_desc *k, const vkn_pipe_src *src) {
    const vkn_drv_cfg *c = vkn_drv();
    if (!c->noout || !k || !src) return 0;
    int any = 0;
    for (int i = 0; i < 8; i++) any |= k->write_mask[i] & 0xf;
    if (!any || (k->fx & VKN_PIPE_FX_DISCARD)) return 0;   // nothing written anyway
    if (!src->fs || (k->fx & VKN_PIPE_FX_NO_FS)) { ADD(noout_nofs, 1); return 1; }   // no fragment stage (xbg_game.cpp draw: !fs)
    const vkn_shader *s = (const vkn_shader *)src->shader;
    const int v = s ? g_facts(s) : -1;
    if (v == 1) { ADD(noout_masked, 1); return 1; }
    if (v == 0) { ADD(noout_kept, 1); return 0; }
    // unknown: a piece's shader (vkn_fx: own modules, src->shader NULL) or no library facts. The mask stays (the stage-5.2 pipeline).
    const uint64_t n = ADD(noout_unknown, 1);
    if (s && n <= 16) { vkn_logf(RED "driver corrections: DM_GFX_NOOUT_MASK: no output facts for shader %u (%.16s / %.16s): colour mask as the key", k->shader, s->key[0], s->key[1]); ADD(reds, 1); }
    return 0;
}
float vkn_drv_bias_scale(void) { return vkn_drv()->bias_x2 ? 2.0f : 1.0f; }
void vkn_drv_bias_note(void) { ADD(bias_x2, 1); }
void vkn_drv_cleared(void) {
    const vkn_drv_cfg *c = vkn_drv();
    if (!c->rebind) return;
    if (vkn_draw_raw_begin) vkn_draw_raw_begin();
    if (ADD(rebinds, 1) == 1) vkn_logf("driver corrections: binds forgotten after vkCmdClearAttachments (driverID %u, DM_GFX_CLEAR_REBIND)", c->driver_id);
}
void vkn_drv_ubo_note(int all_static, uint32_t ro_static, uint32_t ro_dynamic) {
    if (all_static) ADD(ubo_static_shaders, 1);
    if (ro_static) ADD(ubo_ro_static, ro_static);
    if (ro_dynamic) ADD(ubo_ro_dynamic, ro_dynamic);
}

void vkn_drv_stats_get(vkn_drv_stats *s) {
    if (!s) return;
    s->noout_masked = __atomic_load_n(&g_s.noout_masked, __ATOMIC_RELAXED); s->noout_nofs = __atomic_load_n(&g_s.noout_nofs, __ATOMIC_RELAXED);
    s->noout_kept = __atomic_load_n(&g_s.noout_kept, __ATOMIC_RELAXED); s->noout_unknown = __atomic_load_n(&g_s.noout_unknown, __ATOMIC_RELAXED);
    s->bias_x2 = __atomic_load_n(&g_s.bias_x2, __ATOMIC_RELAXED); s->rebinds = __atomic_load_n(&g_s.rebinds, __ATOMIC_RELAXED);
    s->locked = __atomic_load_n(&g_s.locked, __ATOMIC_RELAXED); s->ubo_static_shaders = __atomic_load_n(&g_s.ubo_static_shaders, __ATOMIC_RELAXED);
    s->ubo_ro_static = __atomic_load_n(&g_s.ubo_ro_static, __ATOMIC_RELAXED); s->ubo_ro_dynamic = __atomic_load_n(&g_s.ubo_ro_dynamic, __ATOMIC_RELAXED);
    s->reds = __atomic_load_n(&g_s.reds, __ATOMIC_RELAXED);
}
void vkn_drv_census(void) {
    const vkn_drv_cfg *c = vkn_drv(); vkn_drv_stats s; vkn_drv_stats_get(&s);
    vkn_logf("driver corrections: family %s driverID %u | noout %d: pipelines masked %llu (no fragment stage %llu), kept (outputs) %llu, unknown %llu | "
             "bias x2 %d: pipelines %llu | clear rebind %d: clears %llu | serial %d: creations locked %llu | ubo dynamic %d ro %d: all-static shaders %llu, "
             "blocks kept static %llu, dynamic %llu | red %llu",
             vkn_policy_family_name(c->family), c->driver_id, c->noout, (unsigned long long)s.noout_masked, (unsigned long long)s.noout_nofs,
             (unsigned long long)s.noout_kept, (unsigned long long)s.noout_unknown, c->bias_x2, (unsigned long long)s.bias_x2, c->rebind,
             (unsigned long long)s.rebinds, c->serial, (unsigned long long)s.locked, c->ubo_dyn, c->ubo_ro, (unsigned long long)s.ubo_static_shaders,
             (unsigned long long)s.ubo_ro_static, (unsigned long long)s.ubo_ro_dynamic, (unsigned long long)s.reds);
}
void vkn_drv_test_set(const vkn_drv_cfg *c) {
    pthread_mutex_lock(&g_mu);
    memset(&g_s, 0, sizeof g_s);
    if (c) { g_cfg = *c; g_cfg.resolved = 1; __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE); }
    else __atomic_store_n(&g_ready, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mu);
}
#endif
