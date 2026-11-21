// [DOS2] VK-NATIVE stage 3: the few calls between the host parts that were written apart (each part declares the other side weak).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdint.h>
#include "hle_dos2_vkn_exec.h"
// The pass part reset NVN's viewport and scissor to a target's full size (0x9279a0 / 0x928be0 write the viewport cache but not the
// scissor cache): the exec part's "scissor in effect" must see it as a viewport event (hle_dos2_vkn_pass.h declares this weak).
void dm_vkn_on_nvn_viewport_scissor(int32_t x, int32_t y, int32_t w, int32_t h);
void dm_vkn_on_nvn_viewport_scissor(int32_t x, int32_t y, int32_t w, int32_t h) { (void)x; (void)y; (void)w; (void)h; dm_vkn_exec_viewport_note(); }

// ---- engine textures written through their mapped storage (mode 1) ----
// On an immediate command list (list+0x30 == 0: the render thread's own list) the engine updates a texture by copying rows straight
// into the NVN texture's mapped storage ([texture object + 0xe0] = A.ptr, row pitch from nvnTextureGetStride): no renderer function,
// the native image never sees it (phone runs V3_b..V3_j: the sight maps stayed zero, the post step that multiplies the scene by them
// wrote black). Every such writer asks nvnTextureGetStride (table slot 217, main+0x2bbf560) right before its copy; that slot is ours
// (stor_get_stride): it answers through the old entry and notes (texture object, stride, the writer, the region it writes). The
// notes are mirrored into the native image with vkn_tex_update (recorded in order in the open recording, else queued) when the
// write is known finished: when the writer's hook returns (0x14284c0, 0xff5500), at packet 0x2d (the unlock after a 0x2c), and in
// any case before the next draw (dm_vkn_draw_fill) - the note list is per thread.
//
// The writers (stage 4 RE: decompilations of 0x14284c0 0x1428ba0 0xff5500 0xff5280 0xff5800 0xd1ada0 0x9410f0 0x93e5f0; the slot is
// loaded `adrp x, 0x2bbf000; ldr x, [x, #0x560]; blr x` exactly at the nine return addresses below, nowhere else in main.raw):
//   lr 0x1428aac / 0x1428b18  0x14284c0(o) (called by 0xd63ce0), the two sight maps: handle o+0x70 from the CPU map o+0x50 and
//            handle o+0x78 from o+0x60, each `[o+0x98]` rows of `4 * [o+0x94]` bytes from (0, 0) (R8 textures: 4 texels a "w"). Early
//            out per map: nothing written when its change list (o+0x40 / o+0x48: 16-byte entries {u32 texel index, ...}, count in bits
//            24..47 of +0x10) is empty and the byte o+0xcd is 0; else 0x1428ba0 applies the list to the CPU map and the WHOLE map is
//            copied. So the bytes that change are the listed texels, but the copy is the whole map: mirrored by band diff (below).
//            Immediate only when the caller is the render thread ([[[0x2bc02b8]+0x160]+0x50]+0x28 == the thread id: list +0x68,
//            +0x30 == 0); any other thread writes packet 0x2c / 0x2d instead.
//   lr 0xff5710  0xff5500(o, u8 i, handle, k) (callers 0xff5280 on its dirty flags o+0x70.., 0xff5800): e = [o + 0x10i + 8k + 0x40],
//            `[e+0x24]` rows of `[e+0x28] * [e+0x20]` bytes (bpp x w) from (0, 0), the same immediate / packet split: band diff.
//   lr 0xd1ae50 / 0xd1aef8 / 0xd1afa0 / 0xd1b048  0xd1ada0(p) movie planes 0..3 (handles p+0xb8 + 8k) when p+0x110 is set: frame
//            i = [[p+0xa8]+0x14], entry [p+0xe0] + 0x40i + 0x10k = {src, u32 pitch, u32 rows}: `rows` rows of `pitch` bytes. A new
//            picture every time: the region is mirrored whole (no diff). The movie's own draw follows in the same call (flushes it).
//   lr 0x93f888  packet 0x2c in the executor 0x93e5f0: `[pkt+0x18]` rows of `[pkt+0x14]` bytes, from (pkt + 0x2f) & ~15. The UI
//            part's model of 0x2c (hle_dos2_vkn_ui.c model_2c) uploads the same rows from the packet before the handler, with
//            [pkt+0x1c] (always 4) as texel bytes: right for RGBA8, a quarter of each row for R8 (the sight maps). So: when that model
//            is installed and the texture's texel size is [pkt+0x1c], the note is a duplicate (skipped); otherwise band diff.
//   lr 0x94122c  0x9410f0(list, handle, src, w, h) immediate branch: h rows of 4w bytes. The UI part's hook h_upd uploads w x h from
//            src after the original (texel size 4): a duplicate for 4-byte formats (skipped); any other format: the whole texture.
//   any other return address: the whole texture (the stage-3 behaviour), counted and reported in red.
// The engine's "locked" byte (texture record + 0x17: 1 before the copy, 0 after; packets 0x2c / 0x2d) is a plain store, read only
// by 0x920460 (it is swapped with the record there): no event to hook. The "write finished" events used instead are the writer's
// return (our hooks) and packet 0x2d (our model); the next draw stays the safety net, and dm_vkn_glue_flush_tex can be called at the
// end of the frame (a note left over there = a writer we do not know).
// Band diff (vkn_mirror.h): per texture a 64-bit hash of each 16-row band of the region as last mirrored; only changed bands are
// uploaded. The hashes are forgotten whenever the image may have changed otherwise (a skipped duplicate's upload, a GPU write:
// vkn_tex.pass_written in the generation word, a failed upload, a re-created texture).
// DM_VK_NATIVE_MIRROR (read at install): 2 (default) region + band diff; 1 region only (every write uploads its rows, duplicates
// still skipped); 0 the stage-3 behaviour (the whole texture for every note, duplicates included).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_tex.h"
#include "../gfx/vkn/vkn_read.h"
#include "../gfx/vkn/vkn_mirror.h"
#include "../gfx/vkn/vkn_rt.h"   // [stage 5.5] the recording thread: a flush with notes owns the recording (it records uploads, reads t->pass_written)

#define GL_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own

enum { STOR_SLOT_OFF = 0x2bbf560, STOR_MAX = 32 };
enum { GS_SIGHT, GS_FF, GS_VIDEO, GS_PKT, GS_UPD, GS_OTHER, GS_N };
static const char *const k_gs_name[GS_N] = {"sight maps 0x14284c0", "0xff5500", "movie planes 0xd1ada0", "packet 0x2c", "UpdateTexture 0x9410f0", "unknown writer"};
static const struct { uint32_t lr; uint8_t site, sub; } k_lr[] = {
    {0x1428aac, GS_SIGHT, 0}, {0x1428b18, GS_SIGHT, 1}, {0xff5710, GS_FF, 0},
    {0xd1ae50, GS_VIDEO, 0}, {0xd1aef8, GS_VIDEO, 1}, {0xd1afa0, GS_VIDEO, 2}, {0xd1b048, GS_VIDEO, 3},
    {0x93f888, GS_PKT, 0}, {0x94122c, GS_UPD, 0},
};
enum { NM_DIFF, NM_WHOLE_REGION, NM_PKT, NM_UPD };   // NM_PKT / NM_UPD: duplicate or not, decided at the flush
typedef struct { uint64_t obj; uint32_t stride, rowbytes, rows; uint8_t site, mode, aux, pad; } stor_note;   // rowbytes 0: whole texture

typedef uint64_t (*stor_fn)(uint64_t);
static stor_fn g_stor_orig;
static int g_mir = 2;   // DM_VK_NATIVE_MIRROR
static __thread stor_note t_stor[STOR_MAX];
static __thread uint32_t t_nstor;
// Writer context, set by the writer hooks / the packet model around the engine's code (render thread; 0 = not inside).
static __thread uint64_t t_sight, t_ff, t_video, t_pkt;

static struct {
    uint64_t notes[GS_N], mirrors[GS_N], uploads[GS_N], rows[GS_N], bytes[GS_N], whole[GS_N], dup[GS_N], nothing[GS_N];
    uint64_t unknown, full_list, clamp, bad_fmt, failed, ctx_missing;
    uint32_t red_lr, red_tex, red_up;
} g_gs;

static uint64_t stor_get_stride(uint64_t tex) {   // nvnTextureGetStride(const NVNtexture *): tex = the engine's texture object
    const uint64_t stride = g_stor_orig ? g_stor_orig(tex) : 0;
    if (!tex || vkn_mode() != VKN_NATIVE) return stride;
    const uint64_t lr = (uint64_t)(uintptr_t)__builtin_return_address(0) - (uint64_t)(uintptr_t)dm_vkn_base();
    unsigned site = GS_OTHER, sub = 0;
    for (unsigned i = 0; i < sizeof k_lr / sizeof k_lr[0]; i++) if (k_lr[i].lr == lr) { site = k_lr[i].site; sub = k_lr[i].sub; break; }
    stor_note n; memset(&n, 0, sizeof n);
    n.obj = tex; n.stride = (uint32_t)stride; n.site = (uint8_t)site; n.mode = NM_DIFF;
    // The region the writer copies (its own fields, read before the copy: they do not change during it).
    int32_t rb = 0, rows = 0;
    switch (site) {
    case GS_SIGHT: if (t_sight) { rb = 4 * (int32_t)vkn_rd32(t_sight + 0x94); rows = (int32_t)vkn_rd32(t_sight + 0x98); } break;
    case GS_FF: if (t_ff) { rb = (int32_t)vkn_rd32(t_ff + 0x28) * (int32_t)vkn_rd32(t_ff + 0x20); rows = (int32_t)vkn_rd32(t_ff + 0x24); } break;
    case GS_VIDEO:
        n.mode = NM_WHOLE_REGION;
        if (t_video) {
            const uint64_t bink = vkn_rd64(t_video + 0xa8);
            const uint64_t e = bink ? vkn_rd64(t_video + 0xe0) + 0x40ull * vkn_rd32(bink + 0x14) + 0x10ull * sub : 0;   // (index checked by the engine)
            if (e) { rb = (int32_t)vkn_rd32(e + 8); rows = (int32_t)vkn_rd32(e + 12); }
        }
        break;
    case GS_PKT: n.mode = NM_PKT; if (t_pkt) { rb = (int32_t)vkn_rd32(t_pkt + 0x14); rows = (int32_t)vkn_rd32(t_pkt + 0x18); n.aux = vkn_rd8(t_pkt + 0x1c); } break;
    case GS_UPD: n.mode = NM_UPD; n.aux = 4; break;
    default:
        n.mode = NM_WHOLE_REGION;
        if (g_gs.red_lr++ < 16) vkn_logf(GL_RED "glue: nvnTextureGetStride(object %#llx) from main+%#llx, a writer not in the table: the whole texture is mirrored", (unsigned long long)tex, (unsigned long long)lr);
        break;
    }
    if ((site == GS_SIGHT && !t_sight) || (site == GS_FF && !t_ff) || (site == GS_VIDEO && !t_video) || (site == GS_PKT && !t_pkt)) {
        g_gs.ctx_missing++;   // (the writer's hook is not in: the whole texture, as stage 3 did)
        if (site != GS_PKT) n.mode = NM_WHOLE_REGION;
    } else if (site <= GS_PKT) {
        if (rb <= 0 || rows <= 0) { g_gs.notes[site]++; g_gs.nothing[site]++; return stride; }   // the engine copies nothing either
        n.rowbytes = (uint32_t)rb; n.rows = (uint32_t)rows;
    }
    g_gs.notes[site]++;
    // One note per texture until the flush (the union of the regions: both start at row 0, byte 0).
    uint32_t i = 0;
    while (i < t_nstor && t_stor[i].obj != tex) i++;
    if (i < t_nstor) {
        stor_note *o = &t_stor[i];
        if (!o->rowbytes || !n.rowbytes) o->rowbytes = 0; else { if (n.rowbytes > o->rowbytes) o->rowbytes = n.rowbytes; if (n.rows > o->rows) o->rows = n.rows; }
        if (o->mode != n.mode) { o->mode = NM_WHOLE_REGION; o->site = (uint8_t)site; }
        o->stride = n.stride;
        return stride;
    }
    if (t_nstor < STOR_MAX) { t_stor[t_nstor++] = n; return stride; }
    // Full (a writer we do not know, writing many textures between two draws): the last note gives way, counted.
    g_gs.full_list++;
    t_stor[STOR_MAX - 1] = n;
    return stride;
}

uint64_t dn_rc_model_ops(void) __attribute__((weak));   // hle_dos2_native.c: the opcodes of the first model slot (the UI part's 0x2c)
void dm_vkn_ui_register(void) __attribute__((weak));     // hle_dos2_vkn_ui.c: its 0x9410f0 hook uploads the immediate branch itself

static int up(vkn_tex *t, const stor_note *n, uint64_t src, uint32_t w, uint32_t tb, uint32_t y, uint32_t rows) {
    if (vkn_tex_update(t, 0, 0, 0, (int32_t)y, w, rows, (const void *)(uintptr_t)(src + (uint64_t)y * n->stride), n->stride)) {
        g_gs.failed++;
        if (g_gs.red_up++ < 16) vkn_logf(GL_RED "glue: texture %#llx (%ux%u NVN %#x): upload of rows %u..%u (%u texels, pitch %u) from %s refused", (unsigned long long)t->handle,
                                         t->w, t->h, t->nvn_fmt, y, y + rows, w, n->stride, k_gs_name[n->site]);
        return -1;
    }
    g_gs.uploads[n->site]++; g_gs.rows[n->site] += rows; g_gs.bytes[n->site] += (uint64_t)rows * w * tb;
    return 0;
}
static void flush_one(const stor_note *n) {
    const uint64_t src = vkn_rd64(n->obj + 0xe0);
    vkn_tex *t = vkn_tex_by_cookie(n->obj);
    if (!t || !src || !n->stride) {
        g_gs.unknown++;
        if (g_gs.red_tex++ < 16) vkn_logf(GL_RED "glue: storage write by %s to object %#llx: %s - not mirrored", k_gs_name[n->site], (unsigned long long)n->obj,
                                          !t ? "texture unknown to the backend" : !src ? "no storage pointer" : "stride 0");
        return;
    }
    g_gs.whole[n->site] += (uint64_t)n->stride * t->h;   // what the stage-3 mirror copied for this note
    const uint32_t tb = vkn_read_texel_bytes(t);
    if (g_mir == 0) {   // the stage-3 behaviour
        if (vkn_tex_update(t, 0, 0, 0, 0, t->w, t->h, (const void *)(uintptr_t)src, n->stride)) { g_gs.failed++; return; }
        g_gs.mirrors[n->site]++; g_gs.uploads[n->site]++; g_gs.rows[n->site] += t->h; g_gs.bytes[n->site] += (uint64_t)n->stride * t->h;
        return;
    }
    if (!tb) { g_gs.bad_fmt++; if (g_gs.red_tex++ < 16) vkn_logf(GL_RED "glue: storage write by %s to texture %#llx of NVN format %#x (no 1x1 texels): not mirrored", k_gs_name[n->site], (unsigned long long)t->handle, t->nvn_fmt); return; }
    int mode = n->mode;
    if (mode == NM_PKT) {
        if (dn_rc_model_ops && (dn_rc_model_ops() >> 0x2c & 1) && tb == n->aux) { g_gs.dup[n->site]++; vkn_mirror_forget(n->obj); return; }
        mode = NM_DIFF;
    } else if (mode == NM_UPD) {
        if (dm_vkn_ui_register && tb == n->aux) { g_gs.dup[n->site]++; vkn_mirror_forget(n->obj); return; }
        mode = NM_WHOLE_REGION;
    }
    uint32_t w = n->rowbytes ? n->rowbytes / tb : t->w, rows = n->rowbytes ? n->rows : t->h;
    // (a movie plane's pitch may be padded past the plane's width: only the texture's width is texels, silently)
    if (n->site == GS_VIDEO && w > t->w && (uint64_t)t->w * tb <= n->stride) w = t->w;
    if (w > t->w || rows > t->h || (n->rowbytes && n->rowbytes % tb) || (uint64_t)w * tb > n->stride) {
        g_gs.clamp++;
        if (g_gs.red_tex++ < 16) vkn_logf(GL_RED "glue: %s writes %u rows of %u bytes into texture %#llx (%ux%u, %u bytes a texel, stride %u): clamped to the texture", k_gs_name[n->site],
                                          n->rows, n->rowbytes, (unsigned long long)t->handle, t->w, t->h, tb, n->stride);
        if (w > t->w) w = t->w;
        if (rows > t->h) rows = t->h;
        if ((uint64_t)w * tb > n->stride) w = n->stride / tb;
    }
    if (!w || !rows) return;
    g_gs.mirrors[n->site]++;
    if (mode != NM_DIFF || g_mir == 1) {
        if (up(t, n, src, w, tb, 0, rows)) vkn_mirror_forget(n->obj);
        else if (mode != NM_DIFF) vkn_mirror_forget(n->obj);   // (a whole-region writer: the image no longer matches stored hashes)
        return;
    }
    // Band diff. Generation: the backend texture (re-created -> other pointer / handle / size) and its last GPU write.
    const uint64_t g[5] = {(uint64_t)(uintptr_t)t, t->handle, t->pass_written, (uint64_t)t->w << 32 | t->h, t->nvn_fmt};
    const uint64_t gen = vkn_mirror_hash(g, sizeof g, 0);
    vkn_mirror_run runs[VKN_MIRROR_RUNS]; int full = 0;
    const int nr = vkn_mirror_diff(n->obj, gen, (const uint8_t *)(uintptr_t)src, n->stride, w * tb, rows, runs, VKN_MIRROR_RUNS, &full);
    if (nr < 0) { if (up(t, n, src, w, tb, 0, rows)) vkn_mirror_forget(n->obj); return; }
    if (!nr) g_gs.nothing[n->site]++;
    for (int k = 0; k < nr; k++) if (up(t, n, src, w, tb, runs[k].y, runs[k].rows)) { vkn_mirror_forget(n->obj); break; }
}

void dm_vkn_glue_census(void);
void dm_vkn_glue_repoint(void);
void dm_vkn_glue_repoint(void) {   // after the game filled its NVN table (called by the cmd part's repointing)
    uint64_t *slot = (uint64_t *)(void *)(dm_vkn_base() + STOR_SLOT_OFF);
    const uint64_t cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (!cur || cur == (uint64_t)(uintptr_t)stor_get_stride) return;
    g_stor_orig = (stor_fn)(uintptr_t)cur;
    __atomic_store_n(slot, (uint64_t)(uintptr_t)stor_get_stride, __ATOMIC_RELEASE);
    dm_vkn_census(dm_vkn_glue_census);
    vkn_logf("glue: nvnTextureGetStride (main+%#x) noted: textures the engine writes through their mapped storage are mirrored (DM_VK_NATIVE_MIRROR=%d: %s)", STOR_SLOT_OFF, g_mir,
             g_mir == 2 ? "the writer's rows, changed 16-row bands only" : g_mir == 1 ? "the writer's rows" : "the whole texture, every note");
}
int dm_vkn_glue_is_mine(uint64_t fn);
int dm_vkn_glue_is_mine(uint64_t fn) { return fn == (uint64_t)(uintptr_t)stor_get_stride; }
// Mirrors every texture noted on this thread. Host stack (it records uploads): the draw part before a draw, the writer hooks, the
// packet model (through dm_vkn_on_stack); the frame part may call it at the end of the frame (hle_dos2_vkn_frame.c fh_end, after
// its mu_lock and before vkn_pass_end / present: `if (dm_vkn_glue_flush_tex) dm_vkn_glue_flush_tex();` with a weak declaration).
// Audit U3: the engine destroyed a texture (0x91d630 returned "finalized") that still has a note: the note goes with it (its
// storage is freed). Notes are per thread: the destroy walk of the render thread reaches the render thread's notes.
void dm_vkn_glue_forget(uint64_t obj);
void dm_vkn_glue_forget(uint64_t obj) {
    for (uint32_t i = 0; i < t_nstor; i++)
        if (t_stor[i].obj == obj) { memmove(&t_stor[i], &t_stor[i + 1], sizeof t_stor[0] * (t_nstor - 1 - i)); t_nstor--; i--; }
}
void dm_vkn_glue_flush_tex(void);
static vkn_rt_site g_site_flush = VKN_RT_SITE("storage mirror");
void dm_vkn_glue_flush_tex(void) {
    if (!t_nstor) return;
    stor_note n[STOR_MAX];
    const uint32_t c = t_nstor;
    memcpy(n, t_stor, sizeof n[0] * c); t_nstor = 0;
    vkn_rt_own_begin(&g_site_flush);   // [stage 5.5] in order with the draws before it (vkn_tex_update records into the open recording)
    for (uint32_t i = 0; i < c; i++) flush_one(&n[i]);
    vkn_rt_own_end();
}
static uint64_t flush_on_stack(void *p) { (void)p; dm_vkn_glue_flush_tex(); return 0; }
// Packet 0x2c / 0x2d from the buffer part's model of the executor (guest stack): the packet the next 0x2c handler copies from, and
// the unlock that ends it (the rows are in the storage now).
void dm_vkn_glue_pkt(unsigned op, uint64_t pkt);
void dm_vkn_glue_pkt(unsigned op, uint64_t pkt) {
    if (op == 0x2c) t_pkt = pkt;
    else if (op == 0x2d) { t_pkt = 0; if (t_nstor) dm_vkn_on_stack_x(flush_on_stack, NULL, NULL, DM_VKN_SF_ENC); }   // ([stage 5.5] the flush syncs itself)
}

// ---- the writers' hooks (mode 1): their context for the notes, and the flush when they return (the write is finished) ----
static vkn_fn g_o_sight, g_o_ff, g_o_video;
VKN_HOOK gl_sight(VKN_A) {   // 0x14284c0(o): the sight maps
    const uint64_t prev = t_sight; t_sight = a0;
    const uint64_t rc = g_o_sight(VKN_F);
    t_sight = prev;
    dm_vkn_glue_flush_tex();
    return rc;
}
VKN_HOOK gl_ff(VKN_A) {   // 0xff5500(o, u8 i, handle, k): e = [o + 0x10i + 8k + 0x40] (0 or handle 0: the original returns)
    const uint64_t prev = t_ff;
    t_ff = a0 ? vkn_rd64(a0 + 0x10ull * (a1 & 0xff) + 8ull * a3 + 0x40) : 0;
    const uint64_t rc = g_o_ff(VKN_F);
    t_ff = prev;
    dm_vkn_glue_flush_tex();
    return rc;
}
VKN_HOOK gl_video(VKN_A) {   // 0xd1ada0(p): the movie's planes, then its draw (which flushes the notes: same call, same thread)
    const uint64_t prev = t_video; t_video = a0;
    const uint64_t rc = g_o_video(VKN_F);
    t_video = prev;
    dm_vkn_glue_flush_tex();
    return rc;
}
// Called by the buffer part's register (hle_dos2_vkn_buf.c), mode 1 only.
void dm_vkn_glue_register(void);
void dm_vkn_glue_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;
    { const char *e = getenv("DM_VK_NATIVE_MIRROR"); if (e && *e >= '0' && *e <= '2') g_mir = *e - '0'; }
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_sight[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    static const uint32_t k_ff[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    static const uint32_t k_video[4] = {0xd10283ff /* sub sp,sp,#0xa0 */, 0xa9046ffc, 0xa90567fa, 0xa9065ff8};
    dm_vkn_site(0x14284c0, k_sight, "SightMapsUpload", (void *)gl_sight, &g_o_sight, VKN_M_NATIVE);
    dm_vkn_site(0xff5500, k_ff, "TextureRowsUpload", (void *)gl_ff, &g_o_ff, VKN_M_NATIVE);
    dm_vkn_site(0xd1ada0, k_video, "MoviePlayer::Draw", (void *)gl_video, &g_o_video, VKN_M_NATIVE);
    // [stage 5.5] engine code + the flush (which syncs only when it has notes): no default sync at the entry
    dm_vkn_stack_flag((void *)gl_sight, DM_VKN_SF_ENC); dm_vkn_stack_flag((void *)gl_ff, DM_VKN_SF_ENC); dm_vkn_stack_flag((void *)gl_video, DM_VKN_SF_ENC);
}

void dm_vkn_glue_census(void);
void dm_vkn_glue_census(void) {
    const double M = 1048576.0;
    char s[900]; int k = 0; s[0] = 0;
    uint64_t upb = 0, wh = 0;
    for (int i = 0; i < GS_N; i++) {
        upb += g_gs.bytes[i]; wh += g_gs.whole[i];
        if (!g_gs.notes[i]) continue;
        k += snprintf(s + k, sizeof s - (size_t)k, " %s: %llu notes, %llu mirrored, %llu duplicates skipped, %llu unchanged, %llu uploads %llu rows %.1f MB (whole texture %.1f MB);",
                      k_gs_name[i], (unsigned long long)g_gs.notes[i], (unsigned long long)g_gs.mirrors[i], (unsigned long long)g_gs.dup[i], (unsigned long long)g_gs.nothing[i],
                      (unsigned long long)g_gs.uploads[i], (unsigned long long)g_gs.rows[i], g_gs.bytes[i] / M, g_gs.whole[i] / M);
        if (k >= (int)sizeof s - 1) break;
    }
    vkn_mirror_stats m; vkn_mirror_stats_get(&m);
    vkn_logf("glue: storage-written textures (DM_VK_NATIVE_MIRROR=%d): %.1f MB uploaded of %.1f MB whole-texture;%s not mirrored: unknown %llu, format %llu, refused %llu; clamped %llu, "
             "over the note list %llu, writer hook missing %llu; band diff: %llu diffs (%llu full, %llu unchanged), %llu of %llu bands changed, %llu merged, %.1f MB hashed, %llu forgotten, %llu evicted",
             g_mir, upb / M, wh / M, k ? s : " no notes;", (unsigned long long)g_gs.unknown, (unsigned long long)g_gs.bad_fmt, (unsigned long long)g_gs.failed,
             (unsigned long long)g_gs.clamp, (unsigned long long)g_gs.full_list, (unsigned long long)g_gs.ctx_missing, (unsigned long long)m.diffs, (unsigned long long)m.full,
             (unsigned long long)m.unchanged, (unsigned long long)m.dirty_bands, (unsigned long long)m.bands, (unsigned long long)m.merged, m.hashed_bytes / M,
             (unsigned long long)m.forgot, (unsigned long long)m.evicted);
}
#endif
