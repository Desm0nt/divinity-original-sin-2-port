// [DOS2] VK-NATIVE shaders, the engine side (dm/gfx/vkn/vkn_shader.c gets plain pointers). main = main.raw of update v786432, build
// 4979b200; r = the renderer ([main+0x2bbdc68]). Map: docs/dos2/nr/VK-NATIVE-MAP-1.md ("Запись шейдера", "Объект программы", 0x919090,
// 0x91ce90), VK-NATIVE-MAP-2.md 1.3 / 1.5.
//   0x915ad0 CreateShader(r, vs, fs, cs) -> handle. vs / fs / cs point to {u64 blob, u32 size} (a stage is built when both are non-zero);
//            the record (r+0xc0 table, 0x60 bytes) gets the stage programs from CreateStageProgram 0x919090(dev+8, rec, blob, stage 0 / 1 / 5)
//            at +8 / +0x10 / +0x18. CreateShader never frees the blobs (its callers LoadShaderFile 0x92d320 / 0x931490 / the built-in
//            shaders 0x914e40 do, after it returned), so the hook reads them after the original.
//   Blob (magic 0x19866891 at +0): section count u32 at +0x4c, sections of 0x90 bytes from +0x90: +4 data offset (from the blob), +8 type
//            (0 = control + code, 3 = reflection), +0x30 / +0x3c control offset / size, +0x34 / +0x38 code offset / size (offsets from
//            the section's data). 0x919090 copies the control into a heap buffer it FREES before returning (only the HLE's interned copy
//            inside nvnProgramSetShaders survives) and the code into a class-2 pool block: program object (0x190) +0xc8 CPU pointer of
//            the block, +0xe8 its GPU address = what nvnProgramSetShaders got as the code (GPU addresses are host pointers in this HLE).
//            So the stage key is computed from the blob's control section and the code at +0xe8: the same bytes the translator hashes.
//   Record: +0x20 / +0x28 / +0x30 constant-block arrays of the stages (0x40 each, binding index u8 at +0x35), counts u8 at +0x38..+0x3a.
//   0x91ce90 DestroyShaderIfOld(frame, rec, force) -> bit 0 = destroyed (programs finalized, rec +8..+0x18 zeroed). rec is either the
//            live record (DestroyShader 0xa47ea0, when old enough) or the copy moved to the wait table r+0xdc0 (0x91dde0 copies +0..+0x58;
//            ProcessDeferredDestroys 0x91ad20). The handle in the copy is not reliable, so the record is identified by its program objects
//            (heap pointers, unique while alive): program -> handle map below, filled at creation.
//   0x92d320 LoadShaderFile(path, flag): the engine's 16-byte string (byte 0 bit 0 clear: length byte0 >> 1, characters from +1; set:
//            u32 length at +4, char * at +8) - the file name of the red lines (thread-local bracket; CreateShader runs inside it).
// Stage 4 (mode 1, hle_dos2_vkn_create.c): 0x919090's MemoryPoolGetBufferAddress / ProgramInitialize / SetShaders and 0x91ce90's
// ProgramFinalize are answered by the create part's stubs (program +0xe8 stays the host pointer of the code block, as above).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_shader.h"
#include "hle_dos2_vkn.h"

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
enum { CTL_COPY = 0xe48, NAME_MAX_ = 96, MAP_N = 1 << 16 };

static vkn_fn g_o_create, g_o_destroy, g_o_load;
static uint64_t g_n_create, g_n_stage, g_n_norec, g_n_badblob, g_n_destroy_calls, g_n_destroyed, g_n_unmatched, g_n_mapfull;

// ---- program object -> engine handle (the destroy hook's way back to the handle) ----
typedef struct prog_ent { uint64_t prog, handle; } prog_ent;
static prog_ent *g_map; static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t map_slot(uint64_t p) { return (uint32_t)((p >> 4) * 0x9E3779B97F4A7C15ull >> 48) & (MAP_N - 1); }
static void map_put(uint64_t prog, uint64_t handle) {   // under g_map_mu
    if (!g_map && !(g_map = (prog_ent *)calloc(MAP_N, sizeof *g_map))) return;
    for (uint32_t i = map_slot(prog), n = 0; n < MAP_N; i = (i + 1) & (MAP_N - 1), n++)
        if (!g_map[i].prog || g_map[i].prog == prog) { g_map[i].prog = prog; g_map[i].handle = handle; return; }
    g_n_mapfull++;
}
static int map_take(uint64_t prog, uint64_t *handle) {   // under g_map_mu; linear probing with backward-shift deletion
    if (!g_map) return 0;
    uint32_t i = map_slot(prog), n = 0;
    for (; n < MAP_N && g_map[i].prog != prog; i = (i + 1) & (MAP_N - 1), n++) if (!g_map[i].prog) return 0;
    if (n == MAP_N) return 0;
    *handle = g_map[i].handle;
    for (uint32_t j = (i + 1) & (MAP_N - 1);; j = (j + 1) & (MAP_N - 1)) {
        if (!g_map[j].prog) break;
        const uint32_t h = map_slot(g_map[j].prog);
        if (((j - h) & (MAP_N - 1)) >= ((j - i) & (MAP_N - 1))) { g_map[i] = g_map[j]; i = j; }   // j's home is not between i and j: move it into the hole
    }
    g_map[i].prog = 0; g_map[i].handle = 0;
    return 1;
}
static uint64_t rec_cookie(uint64_t rec) {   // the first program object of a record (0 = none)
    for (int k = 0; k < 3; k++) { const uint64_t p = vkn_rd64(rec + 8 + 8ull * k); if (p) return p; }
    return 0;
}

// ---- LoadShaderFile: the file name of the shaders created inside it ----
static __thread char t_name[NAME_MAX_]; static __thread int t_named;
VKN_HOOK vkn_h_load(VKN_A) {
    char prev[NAME_MAX_]; const int had = t_named;
    if (had) memcpy(prev, t_name, sizeof prev);
    t_named = 0;
    if (a0) {
        uint8_t s[16]; memcpy(s, (const void *)(uintptr_t)a0, sizeof s);   // the engine reads the same 16 bytes next
        uint32_t len; const char *p;
        if (s[0] & 1) { memcpy(&len, s + 4, 4); uint64_t q; memcpy(&q, s + 8, 8); p = (const char *)(uintptr_t)q; }
        else { len = s[0] >> 1; p = (const char *)(a0 + 1); if (len > 15) len = 15; }
        if (p && len) {
            if (len >= NAME_MAX_) { p += len - (NAME_MAX_ - 1); len = NAME_MAX_ - 1; }   // keep the end: the file name
            memcpy(t_name, p, len); t_name[len] = 0; t_named = 1;
        }
    }
    const uint64_t rc = g_o_load(VKN_F);
    t_named = had; if (had) memcpy(t_name, prev, sizeof prev);
    return rc;
}

// ---- CreateShader ----
// The stage's control section in the blob, copied into ctl (CTL_COPY bytes, zero-padded). 1 = found.
static int blob_control(uint64_t blob, uint8_t *ctl) {
    if (vkn_rd32(blob) != 0x19866891u) return 0;
    const uint32_t n = vkn_rd32(blob + 0x4c);
    for (uint32_t i = 0; i < n && i < 64; i++) {
        const uint64_t sec = blob + 0x90 + 0x90ull * i;
        if (vkn_rd32(sec + 8) != 0) continue;
        const uint64_t data = blob + vkn_rd32(sec + 4);
        uint32_t size = vkn_rd32(sec + 0x3c); if (size > CTL_COPY) size = CTL_COPY;
        memset(ctl, 0, CTL_COPY); memcpy(ctl, (const void *)(uintptr_t)(data + vkn_rd32(sec + 0x30)), size);
        return size != 0;
    }
    return 0;
}
VKN_HOOK vkn_h_create(VKN_A) {
    const uint64_t h = g_o_create(VKN_F);
    __atomic_add_fetch(&g_n_create, 1, __ATOMIC_RELAXED);
    if (!vkn_h_gen(h)) return h;
    const uint64_t rec = vkn_rec(a0, 0xc0, 0x60, h);
    if (!rec) { __atomic_add_fetch(&g_n_norec, 1, __ATOMIC_RELAXED); return h; }
    uint8_t *ctl = (uint8_t *)malloc(3 * CTL_COPY);
    if (!ctl) return h;
    vkn_stage_src src[VKN_SRCS]; memset(src, 0, sizeof src);
    const uint64_t arg[3] = {a1, a2, a3};
    for (int k = 0; k < 3; k++) {
        const uint64_t prog = vkn_rd64(rec + 8 + 8ull * k);
        if (!prog || !arg[k]) continue;
        const uint64_t blob = vkn_rd64(arg[k]);
        if (!blob || !vkn_rd32(arg[k] + 8)) continue;
        if (!blob_control(blob, ctl + k * CTL_COPY)) { __atomic_add_fetch(&g_n_badblob, 1, __ATOMIC_RELAXED); continue; }
        src[k].control = ctl + k * CTL_COPY;
        src[k].code = (const void *)(uintptr_t)vkn_rd64(prog + 0xe8);
        src[k].name = t_named ? t_name : NULL;
        const uint64_t blocks = vkn_rd64(rec + 0x20 + 8ull * k); const uint32_t nblk = vkn_rd8(rec + 0x38 + k);
        uint32_t mask = 1u << 31;
        for (uint32_t i = 0; i < nblk && blocks; i++) { const uint32_t b = vkn_rd8(blocks + 0x40ull * i + 0x35); if (b < 31) mask |= 1u << b; }
        src[k].engine_ubo_mask = mask;
        __atomic_add_fetch(&g_n_stage, 1, __ATOMIC_RELAXED);
    }
    const uint64_t cookie = rec_cookie(rec);
    if (!vkn_shader_create(h, src) && cookie) { pthread_mutex_lock(&g_map_mu); map_put(cookie, h); pthread_mutex_unlock(&g_map_mu); }
    free(ctl);
    return h;
}

// ---- DestroyShaderIfOld ----
VKN_HOOK vkn_h_destroy(VKN_A) {
    const uint64_t cookie = a1 ? rec_cookie(a1) : 0;   // before the original zeroes +8..+0x18
    const uint64_t rc = g_o_destroy(VKN_F);
    __atomic_add_fetch(&g_n_destroy_calls, 1, __ATOMIC_RELAXED);
    if ((rc & 1) && cookie) {
        uint64_t h = 0;
        pthread_mutex_lock(&g_map_mu); const int found = map_take(cookie, &h); pthread_mutex_unlock(&g_map_mu);
        if (found) { vkn_shader_destroy(h); __atomic_add_fetch(&g_n_destroyed, 1, __ATOMIC_RELAXED); }
        else __atomic_add_fetch(&g_n_unmatched, 1, __ATOMIC_RELAXED);
    }
    return rc;
}

static void shader_census(void) {
    vkn_shader_census();
    vkn_logf("shader hooks: CreateShader %llu (stages read %llu, no record %llu, blob without control %llu), DestroyShaderIfOld %llu (destroyed %llu, not ours %llu)%s",
             (unsigned long long)g_n_create, (unsigned long long)g_n_stage, (unsigned long long)g_n_norec, (unsigned long long)g_n_badblob,
             (unsigned long long)g_n_destroy_calls, (unsigned long long)g_n_destroyed, (unsigned long long)g_n_unmatched, g_n_mapfull ? " " RED "program map full" : "");
}

// A dead shader's dense id is reused: the pipeline cache (vkn_state.c) must drop every pipeline built with it first. Weak: the
// state part may be absent from a build.
extern void vkn_pipe_forget_shader(uint32_t shader) __attribute__((weak));
static void vkn_shader_dead(uint32_t id, uint64_t handle) { (void)handle; if (vkn_pipe_forget_shader) vkn_pipe_forget_shader(id); }
void dm_vkn_shader_register(void);
void dm_vkn_shader_register(void) {
    static const uint32_t k_create[4] = {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xa9056ffc, 0xa90667fa, 0xa9075ff8};
    static const uint32_t k_destroy[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    static const uint32_t k_load[4] = {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
    vkn_shader_set_dead_fn(vkn_shader_dead);
    dm_vkn_site(0x92d320, k_load, "LoadShaderFile", (void *)vkn_h_load, &g_o_load, VKN_M_BOTH);
    dm_vkn_site(0x915ad0, k_create, "CreateShader", (void *)vkn_h_create, &g_o_create, VKN_M_BOTH);
    dm_vkn_site(0x91ce90, k_destroy, "DestroyShaderIfOld", (void *)vkn_h_destroy, &g_o_destroy, VKN_M_BOTH);
    dm_vkn_census(shader_census);
}
#endif
