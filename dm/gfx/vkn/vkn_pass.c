// [DOS2] VK-NATIVE stage 3: render passes, clears, texture copies (vkn_pass.h; the rule is written at the top of the header).
// No game addresses, no engine record offsets: dm/host/hle_dos2_vkn_pass.c decodes the engine calls (SetRenderTargets 0x9279a0,
// ClearColorTarget 0x928ab0, ClearDepthStencil 0x928be0, texture copy 0x920b00) and passes vkn_tex pointers.
// State (recording thread only):
//   g_cur      the current target set: what NVN would have bound (references held, so a texture the engine destroys stays an object
//              until the set changes; its t->dead says the engine is done with it);
//   g_open     a render pass is open in the recording (g_open_cb / g_open_serial) for the set g_open_set: g_cur itself, or the
//              one-attachment set of a region clear (opened and ended inside vkn_pass_clear_color);
//   per texture (vkn_tex pass_* fields): remembered clears (colour per layer of level 0, up to 4 layers; depth / stencil), "ever
//              written", the frame of the last write.
// Layouts: vkn_tex.layout is one value for the whole image (every barrier here covers every level and layer). Inside a pass the
// attachments are COLOR_ATTACHMENT_OPTIMAL / DEPTH_STENCIL_ATTACHMENT_OPTIMAL; the explicit barrier into that layout is recorded
// before vkCmdBeginRenderPass (vkn_state.c builds passes with initial = final = that layout, initial UNDEFINED when the load op is
// not LOAD: the pass itself discards only the attachment's (level, layer)); sampled textures SHADER_READ_ONLY_OPTIMAL; copies
// TRANSFER_SRC / TRANSFER_DST; storage images GENERAL. A barrier cannot be recorded inside a pass (no self-dependencies), so any
// transition while a pass is open ends it first (the next draw reopens it with LOAD).
// Barriers are conservative (ALL_COMMANDS, memory read / write); [stage 5.2 B1] the tile-GPU policy (DONT_CARE loads for covered / fresh
// targets, DONT_CARE stores by history, fragment-stage read barriers) is the block "stage 5.2 B1" below (switches: vkn_policy.h).
#ifdef DM_GAME_DOS2
#include "vkn_pass.h"
#include "vkn_frame.h"
#include "vkn_state.h"
#include "vkn_prof.h"   // [stage 5 A0] GPU time by pass
#include "vkn_policy.h" // [stage 5.2 B1] switches and family defaults of the load / store policy
#include "vkn_fx.h"     // [stage 5.2 B1] the first-draw facts (pass_fx)
#include "vkn_draw.h"
#include "vkn_buf.h"
#include "vkn_shader.h"
#include "vkn_drv.h"    // [stage 5.3 V2] DM_GFX_CLEAR_REBIND after vkCmdClearAttachments
#include "../nr/nr_cover.h"   // [stage 5.2 B1] NR-10b: position programs, nr_cv_cover
#include "../nr/nr_frame.h"   // [stage 5.2 B1] NR-FRAME: write mask full for the format
#include "../nb/nb_uiarea.h"  // [stage 5.2 B1] draw rect of a later draw of an area pass
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

typedef struct tset { uint32_t n; vkn_tex *c[VKN_PASS_COLORS]; uint32_t layer[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *d; } tset;
static tset g_cur;
static int g_open; static VkCommandBuffer g_open_cb; static uint32_t g_open_serial; static tset g_open_set;
static vkn_pass_info g_info; static VkRect2D g_area;
static vkn_pass_stats g_st; static vkn_pass_stats2 g_st2;
static int g_inited;
static uint32_t g_red_target, g_red_size, g_red_samples, g_red_copy, g_red_clear;

static int red_ok(uint32_t *c) { return (*c)++ < 16; }
// [stage 5 A3] the watched texture of the shadow cache (vkn_pass_watch, end of file): WATCH(t, ev, depth) before a use of t
static vkn_tex *g_watch_t; static vkn_pass_watch_fn g_watch_fn; static int g_watch_in;
#define WATCH(t, ev, d) do { if ((t) == g_watch_t && (t) && g_watch_fn && !g_watch_in) { g_watch_in = 1; g_watch_fn((t), (ev), (d)); g_watch_in = 0; } } while (0)
static uint32_t mipdim(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }
static int has_content(const vkn_tex *t) { return t->pass_ever || t->layout != VK_IMAGE_LAYOUT_UNDEFINED; }   // (uploaded data counts)
static int is_depth(const vkn_tex *t) { return (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0; }
// [stage 5.2 B1] (the policy block below)
static void ident_note(vkn_tex *t); static void b1_on_barrier(vkn_tex *t); static void b1_pass_closing(void); static void b1_pass_closed(void);
static void b1_rec_begin(void); static void hist_read(vkn_tex *t, int how); static void hist_over(vkn_tex *t); static void disarm(void);
static void written(vkn_tex *t) { t->pass_ever = 1; t->pass_written = vkn_frame() + 1; ident_note(t); }

static int same_set(const tset *a, const tset *b) {
    if (a->n != b->n || a->d != b->d) return 0;
    for (uint32_t i = 0; i < a->n; i++)
        if (a->c[i] != b->c[i] || (a->c[i] && (a->layer[i] != b->layer[i] || a->mip[i] != b->mip[i]))) return 0;
    return 1;
}
static int in_set(const tset *s, const vkn_tex *t) {
    if (s->d == t) return 1;
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i] == t) return 1;
    return 0;
}
// Colour slot of (t, level, layer) in s, -1 = none.
static int color_slot(const tset *s, const vkn_tex *t, uint32_t mip, uint32_t layer) {
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i] == t && s->mip[i] == mip && s->layer[i] == layer) return (int)i;
    return -1;
}
static int open_here(void) { return g_open && g_open_cb == vkn_cb() && g_open_serial == vkn_cb_serial() && g_open_cb != VK_NULL_HANDLE; }

// Integer formats take an integer clear value (the engine's floats converted).
static int fmt_int(VkFormat f) {   // 0 float / normalized, 1 unsigned integer, 2 signed integer
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B8G8R8A8_UINT: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_B8G8R8A8_SINT: return 2;
    default: return 0;
    }
}
static VkClearColorValue clear_color(VkFormat f, const float rgba[4]) {
    VkClearColorValue v; const int k = fmt_int(f);
    for (int i = 0; i < 4; i++) {
        if (k == 1) v.uint32[i] = rgba[i] > 0.0f ? (uint32_t)rgba[i] : 0u;
        else if (k == 2) v.int32[i] = (int32_t)rgba[i];
        else v.float32[i] = rgba[i];
    }
    return v;
}

// Whole-image transition from t->layout. Never inside a pass (callers end it first).
static void barrier(VkCommandBuffer cb, vkn_tex *t, VkImageLayout to) {
    const VkImageLayout from = t->layout;
    const VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->image,
        .subresourceRange = {t->aspect, 0, t->mips, 0, t->layers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    t->layout = to;
    g_st.transitions++;
    b1_on_barrier(t);   // [stage 5.2 B1] (a full barrier: no fragment-only visibility left)
}

// [stage 4: queries] a visibility query must end inside the pass instance it began in; results are copied after it (vkn_query.c, weak)
void vkn_query_pass_closing(VkCommandBuffer cb) __attribute__((weak));
void vkn_query_pass_closed(VkCommandBuffer cb) __attribute__((weak));
// [/stage 4: queries]
static void end_open(void) {
    if (g_open && open_here()) {
        b1_pass_closing();   // [stage 5.2 B1] store poison, the first-draw check
        if (vkn_query_pass_closing) vkn_query_pass_closing(g_open_cb);   // [stage 4: queries]
        vkCmdEndRenderPass(g_open_cb);
        vkn_prof_pass_end(g_open_cb);                                     // [stage 5 A0]
        if (vkn_query_pass_closed) vkn_query_pass_closed(g_open_cb);     // [stage 4: queries]
    }
    g_open = 0; g_open_cb = VK_NULL_HANDLE;
    b1_pass_closed();
}
static void on_begin(void) { g_open = 0; g_open_cb = VK_NULL_HANDLE; b1_rec_begin(); }   // a new recording: nothing of the last one is open

// ---- debug aid: DM_VK_NATIVE_DUMP=k1[,k2,k3,k4] - the colour targets of the k-th "heavy" presented frame (40 passes or more: a
// level frame) as small pictures. At the end of that frame every texture that was a colour attachment in it (first 64, in the order
// of first use) is blitted (level 0, layer 0, NEAREST) into a 480x216 RGBA8 image and read back; 4 frames later the pictures are
// written as <DM_VK_NATIVE_DUMP_DIR | DM_RUNTIME_DIR | dm>/vkn_<k>_<nn>_<handle>_<w>x<h>_f<format>_c<class>.ppm. HDR values clamp
// to 0..1; integer formats and depth are skipped. Costs nothing while the switch is unset. ----
enum { DUMP_W = 480, DUMP_H = 216, DUMP_MAX = 64 };
static struct {
    int init, nat, active, off_on, off; uint32_t at[4], heavy, passes_frame, k, off_n;
    vkn_tex *t[DUMP_MAX]; uint32_t nt;
    VkImage img; VkDeviceMemory mem;
    int live; vkn_block b; uint64_t frame; uint32_t n, pk;
    struct { uint64_t h; uint32_t w, hh, fmt, cls; } e[DUMP_MAX];
} g_dump;
static void dump_note(vkn_tex *t) {
    if (!g_dump.active || !t || is_depth(t) || fmt_int(t->format) || g_dump.nt >= DUMP_MAX) return;
    for (uint32_t i = 0; i < g_dump.nt; i++) if (g_dump.t[i] == t) return;
    g_dump.t[g_dump.nt++] = t;
}
static void dump_write(void) {
    const char *d = getenv("DM_VK_NATIVE_DUMP_DIR"); if (!d || !*d) d = getenv("DM_RUNTIME_DIR"); if (!d || !*d) d = "dm";
    for (uint32_t i = 0; i < g_dump.n; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/vkn_%u_%02u_%llx_%ux%u_f%u_c%u.ppm", d, g_dump.pk, i, (unsigned long long)g_dump.e[i].h, g_dump.e[i].w, g_dump.e[i].hh, g_dump.e[i].fmt, g_dump.e[i].cls);
        FILE *f = fopen(path, "wb"); if (!f) { vkn_logf(RED "dump: cannot write %s", path); break; }
        fprintf(f, "P6\n%d %d\n255\n", DUMP_W, DUMP_H);
        const uint8_t *px = (const uint8_t *)g_dump.b.cpu + (size_t)i * DUMP_W * DUMP_H * 4;
        for (uint32_t k = 0; k < DUMP_W * DUMP_H; k++) fwrite(px + 4 * k, 1, 3, f);
        fclose(f);
    }
    vkn_logf("dump: heavy frame %u: %u targets written to %s/vkn_%u_*.ppm", g_dump.pk, g_dump.n, d, g_dump.pk);
    vkn_block_free(&g_dump.b); g_dump.live = 0;
}
static void dump_begin(void) {   // begin listener: a new recording
    if (!g_dump.init) {
        g_dump.init = 1;
        const char *e = getenv("DM_VK_NATIVE_DUMP");
        { const char *o = getenv("DM_VK_NATIVE_DUMP_OFF"); g_dump.off_on = o && atoi(o) > 0; }
        while (e && *e && g_dump.nat < 4) { char *end = NULL; const unsigned long v = strtoul(e, &end, 10); if (v) g_dump.at[g_dump.nat++] = (uint32_t)v; if (end && *end == ',') e = end + 1; else break; }
    }
    if (!g_dump.nat && !g_dump.off_on) return;
    if (g_dump.live && vkn_frame() >= g_dump.frame + 4) dump_write();
    if (g_dump.passes_frame >= 40) g_dump.heavy++;
    g_dump.passes_frame = 0; g_dump.active = 0; g_dump.nt = 0; g_dump.off = 0;
    // DM_VK_NATIVE_DUMP_OFF=1: the first 4 off-frame recordings that draw anything (screenshots, save thumbnails) as dumps 9001...
    if (!vkn_rec_presented() && g_dump.off_on && !g_dump.live && g_dump.off_n < 4) { g_dump.active = 1; g_dump.off = 1; g_dump.k = 9001 + g_dump.off_n; return; }
    if (!vkn_rec_presented() || g_dump.live) return;
    for (int i = 0; i < g_dump.nat; i++) if (g_dump.at[i] == g_dump.heavy + 1 && g_dump.k != g_dump.at[i]) { g_dump.active = 1; g_dump.k = g_dump.at[i]; }
}
static void dump_close(void) {   // close listener (after vkn_pass_end): the frame's targets are final
    if (!g_dump.active) return;
    g_dump.active = 0;
    if (g_dump.off) { if (!g_dump.passes_frame) return; g_dump.off_n++; }
    else if (g_dump.passes_frame < 40) { g_dump.k = 0; return; }   // not a heavy frame after all: the next one is tried
    VkCommandBuffer cb = vkn_cb(); const vkn_dev *dv = vkn_device();
    if (!cb || !dv || !g_dump.nt) return;
    if (!g_dump.img) {
        const VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {DUMP_W, DUMP_H, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (vkCreateImage(dv->dev, &ci, NULL, &g_dump.img) != VK_SUCCESS || vkn_image_mem(g_dump.img, &g_dump.mem, NULL)) { g_dump.img = VK_NULL_HANDLE; return; }
    }
    if (vkn_block_alloc(VKN_ARENA_STAGING, (uint64_t)g_dump.nt * DUMP_W * DUMP_H * 4, 16, &g_dump.b)) return;
    end_open();
    VkImageMemoryBarrier sb = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_dump.img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    g_dump.n = 0;
    for (uint32_t i = 0; i < g_dump.nt; i++) {
        vkn_tex *t = g_dump.t[i];
        if (t->dead || t->layout == VK_IMAGE_LAYOUT_UNDEFINED || t->samples > 1) continue;
        hist_read(t, 5 /* RD_DUMP */);   // [stage 5.2 B1]
        const VkImageLayout prev = t->layout;
        if (prev != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        sb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
        const VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)t->w, (int32_t)t->h, 1}},
                                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {DUMP_W, DUMP_H, 1}}};
        vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_dump.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &sb);
        const VkBufferImageCopy bc = {.bufferOffset = g_dump.b.off + (uint64_t)g_dump.n * DUMP_W * DUMP_H * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {DUMP_W, DUMP_H, 1}};
        vkCmdCopyImageToBuffer(cb, g_dump.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_dump.b.buf, 1, &bc);
        if (prev != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cb, t, prev);
        g_dump.e[g_dump.n].h = t->handle; g_dump.e[g_dump.n].w = t->w; g_dump.e[g_dump.n].hh = t->h; g_dump.e[g_dump.n].fmt = (uint32_t)t->format; g_dump.e[g_dump.n].cls = t->cls;
        g_dump.n++;
    }
    g_dump.live = 1; g_dump.frame = vkn_frame(); g_dump.pk = g_dump.k;
    vkn_logf("dump: heavy frame %u (frame %llu): %u colour targets read back", g_dump.k, (unsigned long long)g_dump.frame, g_dump.n);
}

// For the draw trace of the dump frame (vkn_draw.c): 1 while the targets of this frame are being collected for a dump, and the
// colour target of slot i of the current set (NULL = none).
int vkn_pass_dump_active(void) { return g_dump.active; }
vkn_tex *vkn_pass_color(uint32_t i) { return i < g_cur.n ? g_cur.c[i] : NULL; }

void vkn_pass_init(void) {
    if (g_inited) return;
    g_inited = 1;
    vkn_rec_on_close(vkn_pass_end);
    vkn_rec_on_close(dump_close);
    vkn_rec_on_begin(on_begin);
    vkn_rec_on_begin(dump_begin);
}
void vkn_pass_end(void) { disarm(); end_open(); }

// A render target that was never written and never cleared is about to be read (sampled, or loaded by a pass that blends into it).
// On the Switch its memory is fresh pool memory: zeros. A new VkImage holds garbage, and garbage in a history target (adapted
// luminance, a previous-frame buffer) can be NaN and never leave it again: the phone run V3_b showed the level for a moment and
// then black for good, with "sampled undefined 4". So such a target is filled with zeros first (every level and layer), outside
// any pass. Returns 1 when it filled.
static uint32_t g_red_zero;
static int zero_fill(VkCommandBuffer cb, vkn_tex *t) {
    if (!cb || t->layout != VK_IMAGE_LAYOUT_UNDEFINED || t->pass_ever) return 0;
    if (t->cls != VKN_TC_COLOR && t->cls != VKN_TC_WINDOW && t->cls != VKN_TC_DEPTH) return 0;   // (a sampled texture without data: its format may be compressed)
    end_open();
    barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageSubresourceRange rg = {t->aspect, 0, t->mips, 0, t->layers};
    if (is_depth(t)) { const VkClearDepthStencilValue v = {0.0f, 0}; vkCmdClearDepthStencilImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg); }
    else { VkClearColorValue v; memset(&v, 0, sizeof v); vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg); }
    g_st2.sampled_undefined++;
    if (red_ok(&g_red_zero)) vkn_logf("pass: target %#llx (%ux%u, %u levels, %u layers, format %d, class %u) read before it was ever written: filled with zeros", (unsigned long long)t->handle, t->w, t->h, t->mips, t->layers, (int)t->format, t->cls);
    return 1;
}

// ==== [stage 5.2 B1] load / store policy for tile GPUs (vkn_pass.h "stage 5.2 B1", switches and defaults: vkn_policy.h) ====
// Three rules decide the load / store ops in begin_set, a fourth the stages of a read barrier:
//   cover (DM_VK_NATIVE_LOAD)   the pass opens lazily at its first draw, so that draw is known at vkCmdBeginRenderPass: the effect
//                               function pass_fx (vkn_fx.h, order 900 = after every piece that changes a draw - "gbuf-le-pre" at 1000
//                               only reads; pass_fx never changes `out`) copies the facts
//                               of the draw about to be prepared when no pass is open for the current set; begin_set runs NR-10b's
//                               proof (nr_cover.h: the vertex shader's position program evaluated for the vertices this draw submits,
//                               nr_cv_cover with its 1/4 px margin) on them. Proven whole target: colour attachments whose write mask
//                               covers every component their format stores (nr_frame.h nr_fr_mask_full) begin DONT_CARE, the depth
//                               attachment too when the draw writes depth with ALWAYS and the format has no stencil. Proven part
//                               (dynamic resolution): the pass begins with that rect as its render area (Vulkan keeps what lies outside)
//                               - every later draw / clear of the pass must stay inside, else the pass ends and begins again full, LOAD.
//                               Refusals are counted by reason (k_cv_name). DOOM's first_draw_covers was sometimes wrong (magenta under
//                               poison, KB gpu-mali.md): this proof is NR-10b's (no old-proof shortcut), the draw's own facts only,
//                               and =3 poisons what it proves.
//   fresh (DM_VK_NATIVE_FRESH)  1: a never-written single-subresource target begins with CLEAR to zeros instead of zero_fill + LOAD (the
//                               same zeros: lesson V3_b of zero_fill); 2: + an attachment of shared storage another image of the same
//                               storage identity was written into since its own last write begins DONT_CARE (NR-10 DM_NR_RP_FRESH).
//   store (DM_VK_NATIVE_STORE)  the backend records forward and cannot look ahead, so a store is discarded by HISTORY: key = (the chain of
//                               target sets of this recording's passes so far, slot) - the same pass at the same place of a frame of the
//                               same shape. After the pass, an attachment's content is "pending" until it is read (sampled, loaded by a
//                               later pass, copied from, laid out for present / read-back / transfer, storage, the dump) - the key's
//                               streak drops to 0 - or overwritten whole first (CLEAR / DONT_CARE load of the whole single-subresource
//                               image, a full clear command, a full copy into it) - the streak grows. A streak of N frames
//                               (DM_VK_NATIVE_STORE_FRAMES) discards the store. A read of a discarded content is a violation: counted, a
//                               red line, the key and the texture banned for good (this frame cannot be repaired). 2 = decided and
//                               checked, STORE kept; 3 = STORE and the attachment cleared to magenta / depth 0.5 before the pass ends.
//                               Never discarded: shared storage (the save-thumbnail pattern: a target read only by a rare copy), window
//                               textures, more than one level / layer, the shadow cache's watched atlas, a pass of the dump frame.
//   tile barrier (DM_VK_NATIVE_TILEBAR) a draw whose shader binds textures in the fragment stage only gets its read barriers with the
//                               fragment stages as destination (DM_GFX_TILE_BARRIER, FN-32); such a texture is remembered, and a later
//                               read in SHADER_READ_ONLY by anything else (vertex stage, compute, UI, a piece's own draw) gets a full
//                               barrier first.
static struct {
    vkn_pass_policy req;   // vkn_pass_policy_request (-1 = family default); applied at the first recording begin with a device
    vkn_pass_policy cur;   // in effect (all 0 = stage-4 behaviour: the module default for host tests and other callers)
    int pending, family, fx_reg;
} g_pol;
// first-draw facts (pass_fx -> begin_set)
typedef struct ffacts {
    int armed; uint64_t stamp; tset set;
    const vkn_shader *sh; uint16_t blend, ds, raster, vfmt; uint8_t prim_in, prim, indexed, fx_bad;
    uint32_t first, count;
    uint64_t vb[3]; uint32_t vb_off[3]; uint64_t ib; float alpha_ref, vp[4]; int32_t sc[4];
} ffacts;
static ffacts g_ff;
static int g_frag_only;                       // tile barrier: the draw being prepared samples in the fragment stage only
static int g_area_on, g_area_ok;              // the open pass is an area pass / pass_fx found the draw inside it
static int g_area_src;                        // [B1 + B2] where its area came from: 1 the cover proof, 2 the UI's hint (vkn_pass_area_hint)
static int g_hint_on; static VkRect2D g_hint; // [B1 + B2] the hint for the passes of the current target set
static uint32_t g_open_poison, g_open_dpoison; // store poison of the open pass: colour slot bits / depth (mode 3)
static uint32_t g_cov_chk; static uint64_t g_cov_draws;   // a pass begun DONT_CARE by a rule: its first draw must be recorded
static vkn_pass_stats3 g_s3;
static uint32_t g_red_viol, g_red_lost;
static const char *const k_cv_name[VKN_PCV_N] = {"whole target", "part of the target", "no first-draw facts", "stale facts",
    "effect swapped shader / discard / no fragment stage", "conditional rendering", "multisampled", "shader stages / kill / depth / sample mask / alpha test",
    "no position program", "fragment output missing for a slot", "blending", "write mask short of the format", "depth / stencil test",
    "raster discard / not filled", "primitive / count", "vertex format / stream", "vertex / index bytes", "uniform block", "w != 1 or z outside [0,1]",
    "no pixel in viewport and scissor", "geometry does not cover", "covered but culled", "part too small to pay / area off", "part with a clear / zero target",
    "3D target", "proof narrower than the hinted area"};
const char *vkn_pass_cover_name(int why) { return why >= 0 && why < VKN_PCV_N ? k_cv_name[why] : "?"; }

// ---- the shader facts of the proof: the shader library's (weak; absent in host tests unless they install a stand-in) ----
extern int vkn_shlib_on(void) __attribute__((weak));
extern const struct nr_cv_prog *vkn_shlib_vs_pos(const void *shader) __attribute__((weak));
extern int64_t vkn_shlib_fs_outputs(const void *shader) __attribute__((weak));
extern int vkn_shlib_fs_facts(const void *shader) __attribute__((weak));
extern int vkn_shlib_policy(void) __attribute__((weak));
extern int vkn_query_mode(void) __attribute__((weak));
static int lib_facts(const vkn_shader *s, vkn_pass_cover_sh *o) {
    if (!(vkn_shlib_on && vkn_shlib_on() && vkn_shlib_vs_pos && vkn_shlib_fs_outputs && vkn_shlib_fs_facts)) return -1;
    o->pos = vkn_shlib_vs_pos(s->xs[VKN_SRC_VS]);
    const int64_t out = vkn_shlib_fs_outputs(s->xs[VKN_SRC_FS]); const int ff = vkn_shlib_fs_facts(s->xs[VKN_SRC_FS]);
    if (out < 0 || ff < 0) return -1;
    o->fs_outputs = (uint32_t)out; o->fs_facts = (uint32_t)ff;
    return 0;
}
static int (*g_shfacts)(const struct vkn_shader *s, vkn_pass_cover_sh *o) = lib_facts;
void vkn_pass_cover_facts_fn(int (*fn)(const struct vkn_shader *s, vkn_pass_cover_sh *out)) { g_shfacts = fn ? fn : lib_facts; }

// The policy family: the shader library's decision when it has one, else the same rule over the device's properties (vkn_policy.h).
static int policy_family(void) {
    const int f = vkn_shlib_policy ? vkn_shlib_policy() : -1;
    if (f >= 0) return f;
    const vkn_dev *d = vkn_device();
    return vkn_policy_family_from(d ? d->props.vendorID : 0, d ? d->props.deviceName : NULL, getenv("DM_GFX_SIM_GPU"));
}
static void pass_fx(const vkn_draw_in *in, const vkn_fx_ctx *c, vkn_fx_out *out, void *user);
static void pol_apply(void) {
    if (!g_pol.fx_reg && (g_pol.cur.load || g_pol.cur.tilebar)) g_pol.fx_reg = !vkn_fx_register("pass-cover", pass_fx, NULL, 900);
}
void vkn_pass_policy_set(const vkn_pass_policy *p) {
    if (p) g_pol.cur = *p; else memset(&g_pol.cur, 0, sizeof g_pol.cur);
    if (g_pol.cur.store_frames < 1) g_pol.cur.store_frames = 8;
    g_pol.pending = 0;
    pol_apply();
}
void vkn_pass_policy_request(const vkn_pass_policy *p) { if (p) { g_pol.req = *p; g_pol.pending = 1; } }
void vkn_pass_policy_get(vkn_pass_policy *p, int *family) { if (p) *p = g_pol.cur; if (family) *family = g_pol.family; }
// Resolves a request (the first recording with a device: the family is known then). One line with what is in effect and why.
static void pol_resolve(void) {
    if (!g_pol.pending || !vkn_device()) return;
    const int f = policy_family(); vkn_pass_policy p = g_pol.req;
    const int dl = p.load < 0, ds = p.store < 0, df = p.fresh < 0, dt = p.tilebar < 0;
    if (dl) p.load = (int8_t)vkn_policy_load_default(f);
    if (ds) p.store = (int8_t)vkn_policy_store_default(f);
    if (df) p.fresh = (int8_t)vkn_policy_fresh_default(f);
    if (dt) p.tilebar = (int8_t)vkn_policy_tilebar_default(f);
    if (p.area < 0) p.area = 1;
    if (p.ubo < 0) p.ubo = 1;
    g_pol.family = f;
    vkn_pass_policy_set(&p);
    vkn_logf("pass policy (family %s): DM_VK_NATIVE_LOAD=%d (%s) cover proof of the first draw%s%s, DM_VK_NATIVE_STORE=%d (%s) history %u frames, "
             "DM_VK_NATIVE_FRESH=%d (%s), DM_VK_NATIVE_TILEBAR=%d (%s); %s", vkn_policy_family_name(f), p.load, dl ? "family default" : "env",
             p.area ? ", area passes" : "", p.ubo ? ", positions from uniforms" : "", p.store, ds ? "family default" : "env", (unsigned)p.store_frames,
             p.fresh, df ? "family default" : "env", p.tilebar, dt ? "family default" : "env",
             p.load && !(vkn_shlib_vs_pos && vkn_shlib_on && vkn_shlib_on()) ? "no shader library position programs: every proof refuses (no position program)" : "census line [vkn] pass policy every 10 s");
}

// ---- bytes of one texel (the tile-traffic model of the census: w x h x this per loaded / stored attachment) ----
static uint32_t fmt_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB: case VK_FORMAT_S8_UINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 2;
    case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 8;
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32_SFLOAT: return 12;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;   // RGBA8 / BGRA8 / A2B10G10R10 / B10G11R11 / E5B9G9R9 / RG16 / R32 / D24S8 / X8_D24 / D32
    }
}
static uint64_t att_bytes(const vkn_tex *t, uint32_t w, uint32_t h) { return (uint64_t)w * h * fmt_bytes(t->format) * (t->samples ? t->samples : 1); }
static int single_sub(const vkn_tex *t) { return t->mips == 1 && t->layers == 1 && t->nvn_target != 2; }

// ---- per-texture records (store history, tile-barrier visibility) and per-key history: open addressing, recording thread ----
enum { ST_N = 1024, SK_N = 4096, ID_N = 256, PROBE = 32 };
enum { HS_NONE = 0, HS_PENDING, HS_DEAD, HS_VDEAD };
typedef struct st_ent { const vkn_tex *t; VkImage img; uint64_t key; uint32_t frame; uint8_t state, banned, fragvis, live, keep; } st_ent;   // keep: never discard (a detached depth)
typedef struct sk_ent { uint64_t key; uint32_t frame, inc; uint8_t streak, banned, live, pad; } sk_ent;   // frame: last use; inc: frame + 1 of the last streak step
typedef struct id_ent { uint64_t block, off; const vkn_tex *writer; } id_ent;
static st_ent g_st_tab[ST_N]; static sk_ent g_sk_tab[SK_N]; static id_ent g_id_tab[ID_N];
static uint32_t g_fragvis_n;
static uint64_t g_prefix;
static uint64_t mix64(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }
static uint32_t frame32(void) { return (uint32_t)vkn_frame(); }
// The record of t (create: make one; a record whose image differs belongs to an earlier texture at this address: reset). NULL = none / full.
static st_ent *st_get(const vkn_tex *t, int create) {
    const uint32_t h = (uint32_t)(mix64(0, (uint64_t)(uintptr_t)t) & (ST_N - 1));
    st_ent *free_e = NULL, *old = NULL;
    for (uint32_t i = 0; i < PROBE; i++) {
        st_ent *e = &g_st_tab[(h + i) & (ST_N - 1)];
        if (e->live && e->t == t) {
            if (e->img != t->image) { if (e->fragvis && g_fragvis_n) g_fragvis_n--; memset(e, 0, sizeof *e); if (!create) return NULL; e->live = 1; e->t = t; e->img = t->image; e->frame = frame32(); }
            return e;
        }
        if (!e->live) { if (!free_e) free_e = e; }
        // unused for 600 frames: reusable - also a plain pending store (its key only misses one step); never a discarded one (a late
        // read must still be caught), a ban, or a fragment-only visibility
        else if (!e->banned && !e->keep && (e->state == HS_NONE || e->state == HS_PENDING) && !e->fragvis && frame32() - e->frame > 600 && !old) old = e;
    }
    if (!create) return NULL;
    st_ent *e = free_e ? free_e : old;
    if (!e) { g_s3.table_full++; return NULL; }
    memset(e, 0, sizeof *e); e->live = 1; e->t = t; e->img = t->image; e->frame = frame32();
    return e;
}
static sk_ent *sk_get(uint64_t key, int create) {
    const uint32_t h = (uint32_t)(key & (SK_N - 1));
    sk_ent *free_e = NULL, *old = NULL;
    for (uint32_t i = 0; i < PROBE; i++) {
        sk_ent *e = &g_sk_tab[(h + i) & (SK_N - 1)];
        if (e->live && e->key == key) return e;
        if (!e->live) { if (!free_e) free_e = e; }
        else if (!e->banned && frame32() - e->frame > 600 && (!old || e->frame < old->frame)) old = e;
    }
    if (!create) return NULL;
    sk_ent *e = free_e ? free_e : old;
    if (!e) { g_s3.table_full++; return NULL; }
    memset(e, 0, sizeof *e); e->live = 1; e->key = key; e->frame = frame32();
    return e;
}
// shared storage: the last image written on each identity (fresh rule 2)
static id_ent *id_get(uint64_t block, uint64_t off, int create) {
    const uint32_t h = (uint32_t)(mix64(block, off) & (ID_N - 1));
    for (uint32_t i = 0; i < PROBE; i++) {
        id_ent *e = &g_id_tab[(h + i) & (ID_N - 1)];
        if (e->writer && e->block == block && e->off == off) return e;
        if (!e->writer) { if (!create) return NULL; e->block = block; e->off = off; return e; }
    }
    return NULL;
}
static void ident_note(vkn_tex *t) { if (t->shared && g_pol.cur.fresh >= 2) { id_ent *e = id_get(t->st_block, t->st_off, 1); if (e) e->writer = t; } }
static int alias_clobbered(const vkn_tex *t) {
    if (!t->shared) return 0;
    const id_ent *e = id_get(t->st_block, t->st_off, 0);
    return e && e->writer && e->writer != t;
}

// ---- store history events ----
static const char *const k_read_name[] = {"sampled", "loaded by a later pass", "copied from", "laid out for a transfer / present / read-back", "used as a storage image", "read by the dump"};
enum { RD_SAMPLE, RD_LOAD, RD_COPY, RD_LAYOUT, RD_STORAGE, RD_DUMP };
// The old content of t is read now: a pending store is resolved as "read" (a discarded one is a violation).
static void hist_read(vkn_tex *t, int how) {
    if (!g_pol.cur.store || !t) return;
    st_ent *e = st_get(t, 0);
    if (!e || !e->state) return;
    sk_ent *k = sk_get(e->key, 0);
    if (e->state == HS_DEAD || e->state == HS_VDEAD) {
        const int real = e->state == HS_DEAD;
        if (real) g_s3.violations++; else g_s3.would_violations++;
        g_s3.bans++; e->banned = 1; if (k) k->banned = 1;
        if (red_ok(&g_red_viol))
            vkn_logf(RED "pass: store %s of target %#llx (%ux%u, format %d, class %u, %s) in frame %u is %s in frame %u: %s; the store rule is off for that pass key and this texture from now on",
                     real ? "DONT_CARE" : "DONT_CARE (verify: would have been)", (unsigned long long)t->handle, t->w, t->h, (int)t->format, t->cls, t->sname[0] ? t->sname : "-",
                     e->frame, k_read_name[how], frame32(), real ? "its content is undefined this frame (cannot be repaired)" : "a real discard would have shown garbage");
    }
    if (k) k->streak = 0;
    e->state = HS_NONE;
}
// The whole of t is overwritten before anything read it: a pending store is resolved as "not read".
static void hist_over(vkn_tex *t) {
    if (!g_pol.cur.store || !t) return;
    st_ent *e = st_get(t, 0);
    if (!e || !e->state) return;
    sk_ent *k = sk_get(e->key, 0);
    if (k && k->inc != frame32() + 1) { if (k->streak < 255) k->streak++; k->inc = frame32() + 1; }
    e->state = HS_NONE;
}
static int store_eligible(const vkn_tex *t) {
    return single_sub(t) && !t->shared && t->cls != VKN_TC_WINDOW && t != g_watch_t && !t->dead;
}
// The store op of attachment slot `slot` (VKN_PASS_COLORS = depth) of a pass whose load already resolved t's pending content.
// Returns the op; *poison = 1 for mode 3.
static VkAttachmentStoreOp store_decide(vkn_tex *t, uint32_t slot, int area, int *poison, uint32_t w, uint32_t h) {
    *poison = 0;
    const uint64_t bytes = att_bytes(t, w, h);
    g_s3.att_store++;
    if (!g_pol.cur.store) { g_s3.store_bytes += bytes; return VK_ATTACHMENT_STORE_OP_STORE; }
    if (!store_eligible(t)) { g_s3.store_noelig++; g_s3.store_bytes += bytes; return VK_ATTACHMENT_STORE_OP_STORE; }
    st_ent *e = st_get(t, 1); const uint64_t key = mix64(g_prefix, slot + 1);
    sk_ent *k = sk_get(key, 1);
    if (!e || !k) { g_s3.store_bytes += bytes; return VK_ATTACHMENT_STORE_OP_STORE; }
    if (e->keep) { g_s3.store_keep++; g_s3.store_bytes += bytes; e->key = key; e->frame = frame32(); e->state = HS_PENDING; return VK_ATTACHMENT_STORE_OP_STORE; }
    k->frame = frame32(); e->key = key; e->frame = frame32();
    const int may = !k->banned && !e->banned && k->streak >= g_pol.cur.store_frames && !g_dump.active && !area;
    if (!may) { e->state = HS_PENDING; g_s3.store_bytes += bytes; if (k->banned || e->banned) g_s3.store_banned++; return VK_ATTACHMENT_STORE_OP_STORE; }
    if (g_pol.cur.store == 2) { e->state = HS_VDEAD; g_s3.store_would++; g_s3.store_bytes += bytes; g_s3.store_would_bytes += bytes; return VK_ATTACHMENT_STORE_OP_STORE; }
    e->state = HS_DEAD;
    if (g_pol.cur.store == 3) { *poison = 1; g_s3.store_poison++; g_s3.store_bytes += bytes; return VK_ATTACHMENT_STORE_OP_STORE; }
    g_s3.store_dc++; g_s3.store_saved_bytes += bytes;
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

// ---- the first-draw facts (pass_fx) and the proof ----
static uint64_t draw_stamp(void) {   // draws recorded + every drop of the draw path: moves when a prepared draw ends without reaching vkn_pass_ready
    vkn_draw_stats a; vkn_draw_stats2 b; vkn_draw_stats_get(&a); vkn_draw_stats2_get(&b);
    return a.draws + a.dropped_shader + a.dropped_state + a.dropped_pipe + a.dropped_pass + a.dropped_prim + b.dropped_bind + b.dropped_vb + b.dropped_ib +
           b.dropped_viewport + b.dropped_variant + b.dropped_set + b.dropped_cb + b.scissor_empty + b.placeholder_fail;
}
static void disarm(void) { g_ff.armed = 0; g_frag_only = 0; }
// Which stages the shader samples textures in: 1 = fragment only (every texture / image / effect-texture binding of stage 1).
static struct { const vkn_shader *s; const vkn_bind *b; uint64_t h; uint32_t id; uint8_t v; } g_fo[256];   // (pointer, table, handle, id: a new record at an old address differs)
static int frag_only(const vkn_shader *s) {
    if (!s) return 0;
    const uint32_t i = (uint32_t)(((uintptr_t)s >> 4) & 255);
    if (g_fo[i].s == s && g_fo[i].b == s->bind && g_fo[i].h == s->handle && g_fo[i].id == s->id) return g_fo[i].v;
    int v = 1;
    for (uint32_t k = 0; k < s->nbind && v; k++) {
        const vkn_bind *b = &s->bind[k];
        if (b->kind == VKN_BK_FX_TEX ? b->bundle_stage != 4 : (b->kind == VKN_BK_TEXTURE || b->kind == VKN_BK_IMAGE) && b->stage != 1) v = 0;
    }
    g_fo[i].s = s; g_fo[i].b = s->bind; g_fo[i].h = s->handle; g_fo[i].id = s->id; g_fo[i].v = (uint8_t)v;
    return v;
}
static nb_ua_rect draw_rect(const float vp[4], const int32_t sc[4], uint8_t prim, uint32_t W, uint32_t H) {
    const int32_t sw = sc[2] > 0 ? sc[2] : 0, sh = sc[3] > 0 ? sc[3] : 0;
    return nb_ua_draw_rect(vp[0], vp[1] + vp[3], vp[2], -vp[3], sc[0], sc[1], (uint32_t)sw, (uint32_t)sh, W, H, prim >= 1 && prim <= 4 ? 0 : 4);
}
static void pass_fx(const vkn_draw_in *in, const vkn_fx_ctx *c, vkn_fx_out *out, void *user) {
    (void)user;
    if (g_pol.cur.tilebar) g_frag_only = frag_only(out->shader ? out->shader : c->shader);
    if (!g_pol.cur.load) return;
    if (g_open && open_here() && same_set(&g_open_set, &g_cur)) {   // the pass is open: only an area pass has something to check
        if (!g_area_on) return;
        const nb_ua_rect a = {g_area.offset.x, g_area.offset.y, g_area.offset.x + (int32_t)g_area.extent.width, g_area.offset.y + (int32_t)g_area.extent.height};
        if (nb_ua_contains(a, draw_rect(in->viewport, in->scissor, in->prim, g_info.width, g_info.height))) { g_area_ok = 1; return; }
        end_open(); g_s3.area_restart_draw++;   // this draw begins the pass again (full area): its facts below
    }
    ffacts *f = &g_ff;
    f->armed = 1; f->stamp = draw_stamp(); f->set = g_cur;
    f->sh = c->shader; f->blend = c->blend; f->ds = out->ds ? out->ds : c->ds; f->raster = c->raster; f->vfmt = c->vfmt;
    f->prim_in = in->prim; f->prim = c->prim; f->indexed = c->indexed; f->first = c->first; f->count = c->count;
    f->fx_bad = out->shader || out->discard || out->no_fs;
    memcpy(f->vb, in->vb, sizeof f->vb); memcpy(f->vb_off, in->vb_off, sizeof f->vb_off); f->ib = in->ib; f->alpha_ref = in->alpha_ref;
    memcpy(f->vp, in->viewport, sizeof f->vp); memcpy(f->sc, in->scissor, sizeof f->sc);
}
typedef struct cover_res { int why; nr_cv_rect r; int depth_ok, exact; } cover_res;
static int cover_why(const tset *s, uint32_t W, uint32_t H, uint32_t samples, cover_res *o) {
    const ffacts *f = &g_ff;
    o->depth_ok = 0; o->exact = 0; memset(&o->r, 0, sizeof o->r);
    if (!f->armed) return VKN_PCV_NOFACTS;
    if (!same_set(&f->set, s) || f->stamp != draw_stamp()) return VKN_PCV_STALE;
    if (f->fx_bad) return VKN_PCV_FX;
    if (vkn_query_mode && vkn_query_mode() != 0) return VKN_PCV_COND;   // (a GPU predicate may skip the draw)
    if (samples > 1) return VKN_PCV_SAMPLES;
    const vkn_shader *sh = f->sh;
    const uint8_t need = (uint8_t)((1u << VKN_SRC_VS) | (1u << VKN_SRC_FS));
    if (!sh || (sh->stages & need) != need || ((sh->missing | sh->unusable) & need) || f->alpha_ref >= 0.0f || vkn_fx_shader_is(sh)) return VKN_PCV_SHADER;
    vkn_pass_cover_sh sf; memset(&sf, 0, sizeof sf);
    if (!g_shfacts || g_shfacts(sh, &sf)) return VKN_PCV_NOPROG;
    if (sf.fs_facts & 7u) return VKN_PCV_SHADER;   // VKN_SHLIB_FS_KILL / DEPTH / MASK
    const nr_cv_prog *pr = sf.pos;
    if (!pr || !pr->ok) return VKN_PCV_NOPROG;
    const VkPipelineRasterizationStateCreateInfo *ri = f->raster ? vkn_raster_info(f->raster) : NULL;
    if (f->raster && !ri) return VKN_PCV_RASTER;
    if (ri && (ri->rasterizerDiscardEnable || ri->polygonMode != VK_POLYGON_MODE_FILL)) return VKN_PCV_RASTER;
    const VkPipelineColorBlendAttachmentState *ba = f->blend ? vkn_blend_att(f->blend) : NULL;
    for (uint32_t i = 0; i < s->n; i++) {
        const vkn_tex *t = s->c[i];
        if (!t) continue;
        if (t->nvn_target == 2) return VKN_PCV_3D;
        if (!(sf.fs_outputs >> i & 1u)) return VKN_PCV_OUTPUT;
        if (ba && ba->blendEnable) return VKN_PCV_BLEND;
        if (!nr_fr_mask_full((unsigned)vkn_write_mask_nvn(t->nvn_fmt) & 15u, nr_fr_vk_components((uint32_t)t->format))) return VKN_PCV_MASK;
    }
    if (s->d) {
        const VkPipelineDepthStencilStateCreateInfo *di = f->ds ? vkn_ds_info(f->ds) : NULL;
        if (f->ds && !di) return VKN_PCV_DS;
        if (di && ((di->depthTestEnable && di->depthCompareOp != VK_COMPARE_OP_ALWAYS) || di->stencilTestEnable || di->depthBoundsTestEnable)) return VKN_PCV_DS;
        o->depth_ok = di && di->depthTestEnable && di->depthWriteEnable && !(s->d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    }
    // the vertex ids this draw submits, in the order the rasterizer sees them (fan / quad lists: vkn_draw.c's rewrite of arrays)
    int prim; uint32_t n = 0, vid[NR_CV_MAX_VERTS];
    const uint32_t cnt = f->count;
    if (f->prim_in == 1 || f->prim_in == 3) {
        prim = f->prim_in == 1 ? NR_CV_TRIANGLES : NR_CV_TRIANGLE_STRIP;
        if (cnt < 3 || cnt > NR_CV_MAX_VERTS) return VKN_PCV_PRIM;
        for (uint32_t k = 0; k < cnt; k++) vid[n++] = k;
    } else if ((f->prim_in == 2 || f->prim_in == 4) && !f->indexed) {   // (indexed rewrites lose the first index on the way: refused)
        prim = NR_CV_TRIANGLES;
        if (f->prim_in == 2) { if (cnt < 3 || 3 * (cnt - 2) > NR_CV_MAX_VERTS) return VKN_PCV_PRIM; for (uint32_t k = 0; k + 2 < cnt; k++) { vid[n++] = 0; vid[n++] = k + 1; vid[n++] = k + 2; } }
        else { if (cnt < 4 || 6 * (cnt / 4) > NR_CV_MAX_VERTS) return VKN_PCV_PRIM; for (uint32_t q = 0; q + 4 <= cnt; q += 4) { vid[n++] = q; vid[n++] = q + 1; vid[n++] = q + 2; vid[n++] = q; vid[n++] = q + 2; vid[n++] = q + 3; } }
    } else return VKN_PCV_PRIM;
    // the inputs the program reads: float formats of per-vertex streams, bytes through the buffers' CPU pointers
    const vkn_vfmt *vf = f->vfmt ? vkn_vfmt_info(f->vfmt) : NULL;
    struct { const uint8_t *base; uint64_t size, stride, off; uint32_t nc; } inp[16];
    memset(inp, 0, sizeof inp);
    for (uint32_t m = pr->in_mask; m; m &= m - 1) {
        const uint32_t loc = (uint32_t)__builtin_ctz(m);
        if (loc >= 16 || !vf) return VKN_PCV_INPUT;
        const VkVertexInputAttributeDescription *a = NULL;
        for (uint32_t k = 0; k < vf->nattr; k++) if (vf->attr[k].location == loc) a = &vf->attr[k];
        const uint32_t nc = !a ? 0 : a->format == VK_FORMAT_R32_SFLOAT ? 1 : a->format == VK_FORMAT_R32G32_SFLOAT ? 2 : a->format == VK_FORMAT_R32G32B32_SFLOAT ? 3 : a->format == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : 0;
        if (!nc || a->binding >= VKN_VFMT_STREAMS) return VKN_PCV_INPUT;
        const VkVertexInputBindingDescription *bd = NULL;
        for (uint32_t k = 0; k < vf->nbind; k++) if (vf->bind[k].binding == a->binding) bd = &vf->bind[k];
        if (!bd || bd->inputRate != VK_VERTEX_INPUT_RATE_VERTEX || !bd->stride) return VKN_PCV_INPUT;
        vkn_buf_info bi; memset(&bi, 0, sizeof bi);
        if (!f->vb[a->binding] || vkn_buf_lookup(VKN_BUF_VB, f->vb[a->binding], &bi) || !bi.blk.cpu) return VKN_PCV_BUFFER;
        inp[loc].base = (const uint8_t *)bi.blk.cpu; inp[loc].size = bi.size; inp[loc].stride = bd->stride; inp[loc].off = (uint64_t)f->vb_off[a->binding] + a->offset; inp[loc].nc = nc;
    }
    const uint8_t *ub[NR_CV_MAX_UBO]; size_t ubn[NR_CV_MAX_UBO]; memset(ub, 0, sizeof ub); memset(ubn, 0, sizeof ubn);
    for (uint32_t u = 0; u < pr->nubo && u < NR_CV_MAX_UBO; u++) {
        if (!g_pol.cur.ubo) return VKN_PCV_UBO;
        const vkn_bind *b = NULL;
        for (uint32_t k = 0; k < sh->nbind; k++) if (sh->bind[k].binding == pr->ubo[u]) b = &sh->bind[k];
        if (!b || b->stage != 0) return VKN_PCV_UBO;
        if (b->kind == VKN_BK_UNIFORM) {
            uint32_t off = 0, size = 0; const uint8_t *ring = (const uint8_t *)vkn_ring_cpu();
            if (!ring || vkn_draw_ubo_get(0, b->slot, &off, &size) || !size) return VKN_PCV_UBO;
            ub[u] = ring + off; ubn[u] = b->bytes && b->bytes < size ? b->bytes : size;
        } else if (b->kind == VKN_BK_C1 && sh->c1[VKN_SRC_VS].size && sh->c1[VKN_SRC_VS].cpu) {
            ub[u] = (const uint8_t *)sh->c1[VKN_SRC_VS].cpu; ubn[u] = (size_t)sh->c1[VKN_SRC_VS].size;
        } else return VKN_PCV_UBO;
        if (ubn[u] < pr->ubo_bytes[u]) return VKN_PCV_UBO;
    }
    vkn_buf_info ib; memset(&ib, 0, sizeof ib); uint32_t isz = 0;
    if (f->indexed) {
        if (!f->ib || vkn_buf_lookup(VKN_BUF_IB, f->ib, &ib) || !ib.blk.cpu) return VKN_PCV_BUFFER;
        isz = ib.index_size == 4 ? 4 : 2;
    }
    float P[NR_CV_MAX_VERTS][4];
    for (uint32_t k = 0; k < n; k++) {
        uint64_t v;
        if (f->indexed) {
            const uint64_t at = ((uint64_t)f->first + vid[k]) * isz;
            if (at + isz > ib.size) return VKN_PCV_BUFFER;
            const uint8_t *q = (const uint8_t *)ib.blk.cpu + at;
            if (isz == 2) { uint16_t x; memcpy(&x, q, 2); v = x; } else { uint32_t x; memcpy(&x, q, 4); v = x; }
        } else v = (uint64_t)f->first + vid[k];
        float iv[32][4];
        for (uint32_t m = pr->in_mask; m; m &= m - 1) {
            const uint32_t loc = (uint32_t)__builtin_ctz(m);
            const uint64_t at = inp[loc].off + v * inp[loc].stride;
            if (at + 4u * inp[loc].nc > inp[loc].size) return VKN_PCV_BUFFER;
            iv[loc][0] = iv[loc][1] = iv[loc][2] = 0.0f; iv[loc][3] = 1.0f;
            memcpy(iv[loc], inp[loc].base + at, 4u * inp[loc].nc);
        }
        if (!nr_cv_eval(pr, (const float (*)[4])iv, ub, ubn, P[k])) return VKN_PCV_UBO;
    }
    // the Vulkan viewport the draw path sets (vkn_draw.c: y flipped, negative height); facing: CCW in NDC (y up) is front when
    // the Vulkan front face is CCW and the height is negative, or CW and positive (the y flip reverses the framebuffer winding)
    const float vx = f->vp[0], vy = f->vp[1] + f->vp[3], vw = f->vp[2], vh = -f->vp[3];
    const int cull = ri ? (int)(ri->cullMode & 3u) : NR_CV_CULL_NONE;
    const int front_ccw = (ri && ri->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE) == (vh < 0);
    const int32_t sw = f->sc[2] > 0 ? f->sc[2] : 0, shh = f->sc[3] > 0 ? f->sc[3] : 0;
    const int g = nr_cv_cover((const float (*)[4])P, (int)n, prim, cull, front_ccw, vx, vy, vw, vh, f->sc[0], f->sc[1], (uint32_t)sw, (uint32_t)shh, W, H, &o->r);
    o->exact = pr->exact != 0;
    switch (g) {
    case NR_CVG_OK: return nr_cv_rect_full(o->r, W, H) ? VKN_PCV_FULL : VKN_PCV_AREA;
    case NR_CVG_VERTEX: return VKN_PCV_VERTEX;
    case NR_CVG_EMPTY: return VKN_PCV_EMPTY;
    case NR_CVG_CULLED: return VKN_PCV_CULLED;
    case NR_CVG_GEOMETRY: return VKN_PCV_GEOMETRY;
    default: return VKN_PCV_PRIM;
    }
}
// ---- helpers of begin_set / end_open / the entry points ----
static int fresh_clear_ok(const vkn_tex *t) {   // never written, one subresource: the pass's CLEAR to zeros = zero_fill's zeros everywhere
    return single_sub(t) && t->layout == VK_IMAGE_LAYOUT_UNDEFINED && !t->pass_ever && (t->cls == VKN_TC_COLOR || t->cls == VKN_TC_WINDOW || t->cls == VKN_TC_DEPTH);
}
static const float k_magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
// Inside the open pass: colour slots of `mask` to magenta, the depth attachment (dmask) to depth 0.5 / stencil 0x5a, over r.
static void poison_now(uint32_t mask, int dmask, const tset *s, VkRect2D r, uint64_t *ctr) {
    VkClearAttachment ca[VKN_PASS_COLORS + 1]; uint32_t n = 0;
    for (uint32_t i = 0; i < s->n; i++) if ((mask >> i & 1u) && s->c[i]) { memset(&ca[n], 0, sizeof ca[n]); ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[n].colorAttachment = i; ca[n].clearValue.color = clear_color(s->c[i]->format, k_magenta); n++; }
    if (dmask && s->d) { memset(&ca[n], 0, sizeof ca[n]); ca[n].aspectMask = s->d->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT); ca[n].clearValue.depthStencil.depth = 0.5f; ca[n].clearValue.depthStencil.stencil = 0x5a; n++; }
    if (!n || !r.extent.width || !r.extent.height) return;
    const VkClearRect cr = {r, 0, 1};
    vkCmdClearAttachments(g_open_cb, n, ca, 1, &cr);
    vkn_drv_cleared();   // [stage 5.3 V2] DM_GFX_CLEAR_REBIND
    *ctr += n;
}
// end_open, the pass still open in its recording: the store poison (mode 3), the check that a rule's first draw was recorded
static void b1_pass_closing(void) {
    if (g_open_poison || g_open_dpoison) {
        uint64_t n = 0; poison_now(g_open_poison, (int)g_open_dpoison, &g_open_set, g_area, &n);   // (counted at the decision: store_poison)
    }
    if (g_cov_chk) {
        vkn_draw_stats ds; vkn_draw_stats_get(&ds);
        if (ds.draws == g_cov_draws) {
            g_s3.cover_lost++;
            if (red_ok(&g_red_lost)) vkn_logf(RED "pass: a pass on %#llx (%ux%u) began DONT_CARE for its first draw, which was not recorded (dropped after the pass began): its attachments are undefined this frame",
                                              g_open_set.n && g_open_set.c[0] ? (unsigned long long)g_open_set.c[0]->handle : g_open_set.d ? (unsigned long long)g_open_set.d->handle : 0ull, g_info.width, g_info.height);
        }
    }
}
static void b1_pass_closed(void) { g_area_on = 0; g_area_ok = 0; g_area_src = 0; g_open_poison = g_open_dpoison = 0; g_cov_chk = 0; }
// (a recording begin: the host's request is applied here, outside every draw - registering the effect function inside a running chain
// would move the entries vkn_fx_run is walking)
static void b1_rec_begin(void) { pol_resolve(); b1_pass_closed(); disarm(); g_hint_on = 0; g_prefix = mix64(0x51, vkn_rec_presented() ? 1 : 2); }
// tile barrier bookkeeping: t got a full barrier (barrier()): no fragment-only visibility left
static void b1_on_barrier(vkn_tex *t) {
    if (!g_fragvis_n) return;
    st_ent *e = st_get(t, 0);
    if (e && e->fragvis) { e->fragvis = 0; g_fragvis_n--; }
}
static void barrier(VkCommandBuffer cb, vkn_tex *t, VkImageLayout to);
// The read barrier of a fragment-only draw (DM_GFX_TILE_BARRIER's frag_barrier): what came before is made visible to the fragment
// stages only, so the vertex / tiling work that follows does not wait for it. Remembered on t (a later other read gets a full one).
static void barrier_frag(VkCommandBuffer cb, vkn_tex *t, VkImageLayout to) {
    st_ent *e = st_get(t, 1);
    if (!e) { barrier(cb, t, to); g_s3.tilebar_full++; return; }
    const VkImageLayout from = t->layout;
    const VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->image,
        .subresourceRange = {t->aspect, 0, t->mips, 0, t->layers}};
    const VkPipelineStageFlags frag = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, frag, 0, 0, NULL, 0, NULL, 1, &b);
    t->layout = to;
    g_st.transitions++; g_s3.tilebar_frag++;
    if (!e->fragvis) { e->fragvis = 1; g_fragvis_n++; }
}
// t is read in SHADER_READ_ONLY by something that is not a fragment-only draw: a fragment-only barrier made it visible to the
// fragment stages only, so a full one comes first (outside a pass: the open pass ends).
static void fragvis_fix(vkn_tex *t) {
    st_ent *e = st_get(t, 0);
    if (!e || !e->fragvis) return;
    VkCommandBuffer cb = vkn_cb(); if (!cb) return;
    if (g_open) { end_open(); g_st.breaks_sample++; }
    const VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = t->layout, .newLayout = t->layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->image,
        .subresourceRange = {t->aspect, 0, t->mips, 0, t->layers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    e->fragvis = 0; g_fragvis_n--; g_s3.tilebar_fix++;
}
void vkn_pass_stats3_get(vkn_pass_stats3 *s) { if (s) { *s = g_s3; s->frames = vkn_frame(); } }
// ==== [/stage 5.2 B1 policy block]

// Executes the remembered clears of t as commands of their own (outside any pass). 0 = done or nothing to do; -1 = no recording
// (they stay remembered).
static int flush_clears(vkn_tex *t) {
    if (!t->pass_nclr && !t->pass_dclr) return 0;
    VkCommandBuffer cb = vkn_cb();
    if (!cb) return -1;
    end_open();
    barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (g_dump.active && t->format == VK_FORMAT_R8G8B8A8_UNORM && t->w >= 1000) vkn_logf("trace: remembered clear of %llx runs as a command (%u colour, depth bits %u)", (unsigned long long)(t->handle & 0xffff), t->pass_nclr, (unsigned)t->pass_dclr);
    for (uint32_t i = 0; i < t->pass_nclr; i++) {
        const VkClearColorValue v = clear_color(t->format, t->pass_clr[i].rgba);
        const VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, t->pass_clr[i].layer, 1};
        vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
        g_st.clears_cmd++;
    }
    VkImageAspectFlags a = 0;
    if (t->pass_dclr & 1) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (t->pass_dclr & 2) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    a &= t->aspect;
    if (a) {
        const VkClearDepthStencilValue v = {t->pass_depth, t->pass_stencil};
        const VkImageSubresourceRange rg = {a, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rg);
        g_st.clears_cmd++;
    }
    // [stage 5.2 B1] a whole overwrite of a one-subresource image (its layer 0 / every aspect it has): what was stored is not read
    const int whole = t->mips == 1 && t->layers == 1 && t->nvn_target != 2 &&
                      (is_depth(t) ? a == (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) : t->pass_nclr > 0);
    t->pass_nclr = 0; t->pass_dclr = 0;
    if (whole) hist_over(t);
    written(t);
    return 0;
}

// Remembers a full clear of (level 0, layer) of t (the last one of a layer wins).
static void clr_put(vkn_tex *t, uint32_t layer, const float rgba[4]) {
    for (uint32_t i = 0; i < t->pass_nclr; i++)
        if (t->pass_clr[i].layer == layer) { memcpy(t->pass_clr[i].rgba, rgba, sizeof t->pass_clr[i].rgba); t->pass_clr_ns = vkn_now_ns(); return; }
    if (t->pass_nclr >= 4 && flush_clears(t)) {   // more layers than the record holds and no recording to run them: the oldest is lost
        g_st2.clear_lost++;
        memmove(&t->pass_clr[0], &t->pass_clr[1], sizeof t->pass_clr[0] * 3); t->pass_nclr = 3;
    }
    t->pass_clr[t->pass_nclr].layer = layer; memcpy(t->pass_clr[t->pass_nclr].rgba, rgba, sizeof t->pass_clr[0].rgba); t->pass_nclr++;
    t->pass_clr_ns = vkn_now_ns();
}
// 1 = a remembered clear of that layer (its colour in rgba); take: also removed from the record.
static int clr_find(vkn_tex *t, uint32_t layer, float rgba[4], int take) {
    for (uint32_t i = 0; i < t->pass_nclr; i++)
        if (t->pass_clr[i].layer == layer) {
            if (rgba) memcpy(rgba, t->pass_clr[i].rgba, sizeof t->pass_clr[i].rgba);
            if (take) { memmove(&t->pass_clr[i], &t->pass_clr[i + 1], sizeof t->pass_clr[0] * (t->pass_nclr - 1 - i)); t->pass_nclr--; }
            return 1;
        }
    return 0;
}

// Opens a pass for s in vkn_cb() (ending the open one). 0 = open, g_info / g_area filled; -1 = dropped (counted).
// [stage 5.2 B1] load / store ops by the policy block above (cover proof, fresh targets, store history); consumes the first-draw facts.
static uint32_t g_red_zclr;
static int begin_set(const tset *s) {
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; g_ff.armed = 0; return -1; }
    end_open();
    if (vkn_upload_pending_bytes()) (void)vkn_upload_run(cb);   // (a queued creation upload of an attachment must not land after this pass: see vkn_pass_copy)
    vkn_rp_key k; vkn_rp_key_init(&k);
    VkImageView views[VKN_PASS_COLORS + 1]; VkClearValue cv[VKN_PASS_COLORS + 1]; uint32_t nv = 0;
    memset(cv, 0, sizeof cv);
    uint32_t W = ~0u, H = ~0u, samples = 0, ncolor = 0, wmin = 0, wmax = 0; int any = 0, bad = 0;
    vkn_pass_info inf; memset(&inf, 0, sizeof inf);
    // check every attachment before anything is recorded
    for (uint32_t i = 0; i <= s->n && !bad; i++) {
        vkn_tex *t = i < s->n ? s->c[i] : s->d; const int depth = i == s->n;
        if (!t) continue;
        const uint32_t mip = depth ? 0 : s->mip[i], layer = depth ? 0 : s->layer[i];
        if (t->dead) { g_st2.dead_target++; bad = 1; break; }
        if (is_depth(t) != depth) { g_st2.bad_target++; bad = 1; if (red_ok(&g_red_target)) vkn_logf(RED "pass: %s slot %u holds a %s texture (format %d, handle %#llx)", depth ? "depth" : "colour", i, depth ? "colour" : "depth", (int)t->format, (unsigned long long)t->handle); break; }
        VkImageView v = vkn_tex_att_view(t, mip, layer);
        if (!v) { g_st2.bad_target++; bad = 1; if (red_ok(&g_red_target)) vkn_logf(RED "pass: no attachment view of %#llx (class %u, target %u, level %u of %u, layer %u of %u)", (unsigned long long)t->handle, t->cls, t->nvn_target, mip, t->mips, layer, t->layers); break; }
        if (samples && samples != t->samples) { g_st2.samples_mismatch++; bad = 1; if (red_ok(&g_red_samples)) vkn_logf(RED "pass: attachments of %u and %u samples (handle %#llx)", samples, t->samples, (unsigned long long)t->handle); break; }
        samples = t->samples;
        const uint32_t w = mipdim(t->w, mip), h = mipdim(t->h, mip);
        if (any && (w != W || h != H)) wmin = 1;
        if (w < W) W = w; if (h < H) H = h;
        wmax = wmax > w ? wmax : w;
        any = 1;
        views[nv++] = v;
        if (!depth) ncolor = i + 1;
    }
    if (bad) { g_ff.armed = 0; return -1; }
    if (!any) { g_st2.no_targets++; g_ff.armed = 0; return -1; }
    if (wmin) { g_st2.size_mismatch++; if (red_ok(&g_red_size)) vkn_logf(RED "pass: attachments of different sizes: rendering into %ux%u (the smallest; widest %u)", W, H, wmax); }
    // [B1] the cover proof of the first draw (its facts are used once)
    cover_res cr; memset(&cr, 0, sizeof cr); int cw = -1;
    if (g_pol.cur.load) { cw = cover_why(s, W, H, samples ? samples : 1, &cr); g_s3.cover_tried++; }
    // (an engine draw begins this pass: the effect chain saw it - its rect, for the hint below)
    const int eng = g_ff.armed && same_set(&g_ff.set, s) && g_ff.stamp == draw_stamp();
    const nb_ua_rect er = eng ? draw_rect(g_ff.vp, g_ff.sc, g_ff.prim_in, W, H) : nb_ua_none();
    g_ff.armed = 0;
    // [B1 + B2] the render area has two sources. The UI's hint H (vkn_pass_area_hint, vkn_ui.c level 2: every draw of the pass stays
    // inside H - the UI ends the pass before one that does not - with LOAD / STORE semantics: the texels outside H are kept) and the
    // cover proof's rect P (the first draw writes every texel of P; outside P it writes nothing). They are not intersected: a pass begins
    //   with H when hinted - clipped to the attachments; full instead when an attachment begins with a remembered CLEAR (a CLEAR clears
    //     only the area) or under a poison mode - and its attachments begin DONT_CARE only when P contains H (proof for the whole area);
    //   else with P when the proof covers a part of the target (the cover rules: it pays, nothing begins with a clear, no never-written
    //     attachment);
    //   else full.
    // In a narrowed pass a never-written attachment takes the zero fill (all of it zero) instead of the CLEAR to zeros (the area only),
    // and its stores are never discarded (the history only knows whole overwrites).
    int hinted = 0; VkRect2D hint = {{0, 0}, {W, H}};
    if (g_hint_on && s == &g_cur) {
        int64_t x0 = g_hint.offset.x, y0 = g_hint.offset.y, x1 = x0 + g_hint.extent.width, y1 = y0 + g_hint.extent.height;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int64_t)W) x1 = W;
        if (y1 > (int64_t)H) y1 = H;
        int clr = 0;
        for (uint32_t i = 0; i < s->n; i++) if (s->c[i]) clr |= s->mip[i] == 0 && clr_find(s->c[i], s->layer[i], NULL, 0);
        if (s->d) clr |= (s->d->pass_dclr & 3) != 0;
        const nb_ua_rect hr = {(int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1};
        if (x1 <= x0 || y1 <= y0) g_s3.hint_empty++;
        else if (eng && !nb_ua_contains(hr, er)) g_s3.hint_engine++;   // the hint speaks for the UI's draws: an engine draw outside it gets the full area
        else if (clr) g_s3.hint_clear++;
        else if (g_pol.cur.load == 3 || g_pol.cur.store == 3 || g_pol.cur.fresh == 3) g_s3.hint_poison++;
        else { hinted = 1; hint.offset.x = (int32_t)x0; hint.offset.y = (int32_t)y0; hint.extent.width = (uint32_t)(x1 - x0); hint.extent.height = (uint32_t)(y1 - y0); }
    }
    if (hinted && (cw == VKN_PCV_FULL || cw == VKN_PCV_AREA) &&
        !(cr.r.x0 <= hint.offset.x && cr.r.y0 <= hint.offset.y && cr.r.x1 >= hint.offset.x + (int32_t)hint.extent.width && cr.r.y1 >= hint.offset.y + (int32_t)hint.extent.height))
        cw = VKN_PCV_HINT;   // the proof does not cover the hinted area: LOAD
    if (cw == VKN_PCV_AREA && !hinted) {   // a part: render area + DONT_CARE, only when it pays and nothing of the pass begins with a clear
        const uint64_t px = (uint64_t)(cr.r.x1 - cr.r.x0) * (uint64_t)(cr.r.y1 - cr.r.y0), full = (uint64_t)W * H;
        int clr = 0;
        for (uint32_t i = 0; i < s->n; i++) if (s->c[i]) clr |= (s->mip[i] == 0 && clr_find(s->c[i], s->layer[i], NULL, 0)) || !has_content(s->c[i]);
        if (s->d) clr |= (s->d->pass_dclr & 3) != 0 || !has_content(s->d);
        if (!g_pol.cur.area || px * 1000 > full * 900) cw = VKN_PCV_SMALL;
        else if (clr) cw = VKN_PCV_AREA_CLEAR;
    }
    if (cw >= 0) { g_s3.cover_why[cw]++; if ((cw == VKN_PCV_FULL || cw == VKN_PCV_AREA) && cr.exact) g_s3.cover_exact++; }
    const int proven = cw == VKN_PCV_FULL || cw == VKN_PCV_AREA, apply = g_pol.cur.load == 1 || g_pol.cur.load == 3;
    VkRect2D area = hinted ? hint : (VkRect2D){{0, 0}, {W, H}};
    if (cw == VKN_PCV_AREA && apply && !hinted) { area.offset.x = cr.r.x0; area.offset.y = cr.r.y0; area.extent.width = (uint32_t)(cr.r.x1 - cr.r.x0); area.extent.height = (uint32_t)(cr.r.y1 - cr.r.y0); }
    const int is_area = area.extent.width != W || area.extent.height != H;
    const uint32_t AW = area.extent.width, AH = area.extent.height;
    // [B1] the store-history key of this pass: the chain of the recording's target sets so far
    { uint64_t h = (uint64_t)s->n; for (uint32_t i = 0; i < s->n; i++) h = mix64(h, (uint64_t)(uintptr_t)s->c[i] ^ ((uint64_t)s->layer[i] << 48) ^ ((uint64_t)s->mip[i] << 56));
      g_prefix = mix64(g_prefix, mix64(h, (uint64_t)(uintptr_t)s->d)); }
    uint32_t poison_cov = 0, poison_alias = 0, spoison = 0; int dpoison_cov = 0, dpoison_alias = 0, dspoison = 0, chk = 0;
    // load / store ops (the rule of vkn_pass.h + stage 5.2 B1) and the layouts, before the pass begins
    nv = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        vkn_tex *t = s->c[i];
        if (!t) continue;
        float rgba[4]; const uint64_t bytes = att_bytes(t, AW, AH);
        if (s->mip[i] == 0 && clr_find(t, s->layer[i], rgba, 0)) { k.load[i] = VK_ATTACHMENT_LOAD_OP_CLEAR; cv[nv].color = clear_color(t->format, rgba); g_st.clear++; g_st.clears_folded++; g_s3.att_nload[VKN_PDC_CLEAR]++; hist_over(t); }
        else if (proven && apply) {   // the first draw overwrites every component of every pixel of the (proven) area
            if (!has_content(t) && (!single_sub(t) || is_area)) zero_fill(cb, t);   // (the texels it does not write: zeros, V3_b)
            k.load[i] = VK_ATTACHMENT_LOAD_OP_DONT_CARE; g_st.dontcare++; g_s3.att_nload[is_area ? VKN_PDC_AREA : VKN_PDC_COVER]++; g_s3.load_saved_bytes += bytes; chk = 1;
            if (g_pol.cur.load == 3) poison_cov |= 1u << i;
            if (is_area) hist_read(t, RD_LOAD); else hist_over(t);   // (an area pass keeps the old texels outside it)
        }
        else if (has_content(t)) {
            if (g_pol.cur.fresh >= 2 && alias_clobbered(t)) {   // its shared storage holds another image's bytes now (NR-10 "fresh")
                k.load[i] = VK_ATTACHMENT_LOAD_OP_DONT_CARE; g_st.dontcare++; g_s3.att_nload[VKN_PDC_ALIAS]++; g_s3.load_saved_bytes += bytes; chk = 1;
                if (g_pol.cur.fresh == 3) poison_alias |= 1u << i;
                if (is_area) hist_read(t, RD_LOAD); else hist_over(t);
            } else {
                k.load[i] = VK_ATTACHMENT_LOAD_OP_LOAD; g_st.load++; if (t->pass_written != vkn_frame() + 1) g_st2.load_stale++;
                g_s3.att_load++; g_s3.load_bytes += bytes; if (proven) { g_s3.would_dc++; g_s3.would_bytes += bytes; }
                hist_read(t, RD_LOAD);
            }
        }
        else if (g_pol.cur.fresh >= 1 && fresh_clear_ok(t) && !is_area) {   // never written: zeros, as fresh memory on the Switch - by the load op
            k.load[i] = VK_ATTACHMENT_LOAD_OP_CLEAR; memset(&cv[nv].color, 0, sizeof cv[nv].color); g_st.clear++; g_s3.att_nload[VKN_PDC_ZERO]++; g_s3.load_saved_bytes += bytes;
            g_st2.sampled_undefined++;
            if (red_ok(&g_red_zclr)) vkn_logf("pass: target %#llx (%ux%u, format %d, class %u) loaded before it was ever written: its pass begins with a clear to zeros", (unsigned long long)t->handle, t->w, t->h, (int)t->format, t->cls);
        }
        else if (zero_fill(cb, t)) { k.load[i] = VK_ATTACHMENT_LOAD_OP_LOAD; g_st.load++; g_s3.att_load++; g_s3.load_bytes += bytes; }   // never written: zeros, as fresh memory on the Switch
        else { k.load[i] = VK_ATTACHMENT_LOAD_OP_DONT_CARE; g_st.dontcare++; g_s3.att_nload[VKN_PDC_NEVER]++; }
        int sp = 0; k.store[i] = (uint8_t)store_decide(t, i, is_area, &sp, AW, AH); if (sp) spoison |= 1u << i;
        k.color[i] = (uint32_t)t->format;
        inf.write_mask[i] = vkn_write_mask_nvn(t->nvn_fmt);
        if (t->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) barrier(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        nv++;
    }
    k.ncolor = (uint8_t)ncolor;
    k.samples = (uint8_t)(samples ? samples : 1);
    if (s->d) {
        vkn_tex *t = s->d; const uint64_t bytes = att_bytes(t, AW, AH);
        int zclr = 0;
        if (!has_content(t) && (t->pass_dclr & 3) != 3) {   // an aspect that is neither cleared nor ever written: zeros
            if (g_pol.cur.fresh >= 1 && fresh_clear_ok(t) && !is_area) zclr = 1; else zero_fill(cb, t);
        }
        const int hd = (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, hs = (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0, content = has_content(t);
        k.depth = (uint32_t)t->format;
        k.depth_load = ((t->pass_dclr & 1) || zclr) && hd ? VK_ATTACHMENT_LOAD_OP_CLEAR : content ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        k.stencil_load = ((t->pass_dclr & 2) || zclr) && hs ? VK_ATTACHMENT_LOAD_OP_CLEAR : content ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        int why = -1;
        if (k.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD && !hs && proven && cr.depth_ok) {   // the first draw writes depth everywhere (ALWAYS + write)
            if (apply) { k.depth_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE; why = is_area ? VKN_PDC_AREA : VKN_PDC_COVER; g_s3.cover_depth++; if (g_pol.cur.load == 3) dpoison_cov = 1; }
            else { g_s3.would_dc++; g_s3.would_bytes += bytes; }
        }
        else if (k.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD && (!hs || k.stencil_load == VK_ATTACHMENT_LOAD_OP_LOAD) && g_pol.cur.fresh >= 2 && alias_clobbered(t)) {
            k.depth_load = k.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE; why = VKN_PDC_ALIAS; if (g_pol.cur.fresh == 3) dpoison_alias = 1;
        }
        if (why >= 0) { g_st.dontcare++; g_s3.att_nload[why]++; g_s3.load_saved_bytes += bytes; chk = 1; }
        else if (k.depth_load == VK_ATTACHMENT_LOAD_OP_CLEAR || k.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            cv[nv].depthStencil.depth = (t->pass_dclr & 1) ? t->pass_depth : 0.0f; cv[nv].depthStencil.stencil = (t->pass_dclr & 2) ? t->pass_stencil : 0u;
            g_st.clear++; if (t->pass_dclr & 3) g_st.clears_folded++;
            g_s3.att_nload[zclr && !(t->pass_dclr & 3) ? VKN_PDC_ZERO : VKN_PDC_CLEAR]++;
            if (zclr) { g_st2.sampled_undefined++; if (red_ok(&g_red_zclr)) vkn_logf("pass: depth target %#llx (%ux%u, format %d) loaded before it was ever written: its pass begins with a clear to zeros", (unsigned long long)t->handle, t->w, t->h, (int)t->format); }
            if ((hd && k.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD) || (hs && k.stencil_load == VK_ATTACHMENT_LOAD_OP_LOAD)) { g_s3.load_bytes += bytes; g_s3.att_load++; }
        } else if (content) { g_st.load++; if (t->pass_written != vkn_frame() + 1) g_st2.load_stale++; g_s3.att_load++; g_s3.load_bytes += bytes; }
        else { g_st.dontcare++; g_s3.att_nload[VKN_PDC_NEVER]++; }
        // (an aspect the format lacks loads nothing: vkn_state.c makes its ops DONT_CARE)
        if ((hd && k.depth_load == VK_ATTACHMENT_LOAD_OP_LOAD) || (hs && k.stencil_load == VK_ATTACHMENT_LOAD_OP_LOAD) || (why >= 0 && is_area)) hist_read(t, RD_LOAD); else hist_over(t);
        int sp = 0; k.depth_store = k.stencil_store = (uint8_t)store_decide(t, VKN_PASS_COLORS, is_area, &sp, AW, AH); dspoison = sp;
        if (t->layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) barrier(cb, t, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        inf.has_depth = (uint8_t)hd; inf.has_stencil = (uint8_t)hs; inf.depth_format = t->format;
        nv++;
    }
    VkRenderPass rp = VK_NULL_HANDLE; uint16_t compat = 0;
    if (!vkn_rp_get(&k, &rp, &compat) || !rp) { g_st2.rp_fail++; return -1; }
    // views again in attachment order (bound colour slots, then depth): the same as checked above
    nv = 0;
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i]) views[nv++] = vkn_tex_att_view(s->c[i], s->mip[i], s->layer[i]);
    if (s->d) views[nv++] = vkn_tex_att_view(s->d, 0, 0);
    VkFramebuffer fb = vkn_fb_get(compat, views, nv, W, H, 1);
    if (!fb) { g_st2.fb_fail++; return -1; }
    const VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                      .renderArea = area, .clearValueCount = nv, .pClearValues = cv};
    vkn_prof_pass_begin(cb, s->c, s->n, s->d, W, H);   // [stage 5 A0] (after every failure point above)
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g_open = 1; g_open_cb = cb; g_open_serial = vkn_cb_serial(); g_open_set = *s;
    g_area = bi.renderArea;
    // [B1] poison of what a rule began DONT_CARE (the first draw must paint the proven rect over; an alias's readers show magenta)
    if (poison_cov || dpoison_cov) poison_now(poison_cov, dpoison_cov, s, cw == VKN_PCV_AREA || hinted ? area : (VkRect2D){{cr.r.x0, cr.r.y0}, {(uint32_t)(cr.r.x1 - cr.r.x0), (uint32_t)(cr.r.y1 - cr.r.y0)}}, &g_s3.poison_load);
    if (poison_alias || dpoison_alias) poison_now(poison_alias, dpoison_alias, s, area, &g_s3.poison_load);
    g_area_on = is_area; g_area_ok = 0; g_area_src = !is_area ? 0 : hinted ? 2 : 1; g_open_poison = spoison; g_open_dpoison = (uint32_t)dspoison;
    if (is_area) { if (hinted) g_s3.hint_passes++; else g_s3.area_passes++; }
    g_cov_chk = (uint32_t)chk; if (chk) { vkn_draw_stats ds; vkn_draw_stats_get(&ds); g_cov_draws = ds.draws; }
    // the remembered clears this pass performs leave the records only now (a pass that failed above keeps them)
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i] && k.load[i] == VK_ATTACHMENT_LOAD_OP_CLEAR) clr_find(s->c[i], s->layer[i], NULL, 1);
    if (s->d) s->d->pass_dclr = 0;   // (an aspect the format lacks has nothing to clear)
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i]) written(s->c[i]);
    if (s->d) written(s->d);
    inf.pass = rp; inf.compat = compat; inf.ncolor = ncolor; inf.width = W; inf.height = H; inf.samples = k.samples;
    g_info = inf;
    g_st.passes++;
    g_dump.passes_frame++;
    if (g_dump.active && s->n && s->c[0] && s->c[0]->format == VK_FORMAT_R8G8B8A8_UNORM && s->c[0]->w >= 1000)
        vkn_logf("trace: pass begins on %llx (%ux%u) load %d, depth %llx load %d, area %ux%u", (unsigned long long)(s->c[0]->handle & 0xffff), s->c[0]->w, s->c[0]->h, (int)k.load[0], s->d ? (unsigned long long)(s->d->handle & 0xffff) : 0ull, (int)k.depth_load, W, H);
    if (g_dump.active) for (uint32_t i = 0; i < s->n; i++) dump_note(s->c[i]);
    return 0;
}

void vkn_pass_targets(uint32_t n, vkn_tex *const color[VKN_PASS_COLORS], const uint32_t layer[VKN_PASS_COLORS], const uint32_t mip[VKN_PASS_COLORS], vkn_tex *depth) {
    vkn_pass_init();
    disarm();   // [stage 5.2 B1]
    tset s; memset(&s, 0, sizeof s);
    s.n = n > VKN_PASS_COLORS ? VKN_PASS_COLORS : n;
    for (uint32_t i = 0; i < s.n; i++) {
        s.c[i] = color ? color[i] : NULL;
        if (s.c[i]) { s.layer[i] = layer ? layer[i] : 0; s.mip[i] = mip ? mip[i] : 0; }
    }
    s.d = depth;
    if (same_set(&s, &g_cur)) {   // the open pass (if any) stays open - unless it is narrowed ([B1 + B2]: the caller may draw anywhere next)
        if (g_open && g_area_on) { end_open(); g_s3.area_restart_targets++; }
        return;
    }
    end_open();
    g_hint_on = 0;   // [B1 + B2] a hint belongs to the set it was given for
    // [B1 + B2] the same colours with and without a depth target (the UI's depth-stencil detach, vkn_ui.c level 1): the depth's content
    // must survive the detached passes for a later re-attach with LOAD, whatever the history saw before - its store is never discarded
    if (g_pol.cur.store && s.d != g_cur.d && s.n == g_cur.n) {
        int same_c = 1;
        for (uint32_t i = 0; i < s.n && same_c; i++) same_c = s.c[i] == g_cur.c[i] && (!s.c[i] || (s.layer[i] == g_cur.layer[i] && s.mip[i] == g_cur.mip[i]));
        for (int j = 0; j < 2 && same_c && s.n; j++) {
            vkn_tex *d = j ? g_cur.d : s.d; st_ent *e = d ? st_get(d, 1) : NULL;
            if (e && !e->keep) { e->keep = 1; g_s3.store_keep_marked++; }
        }
    }
    // references: the new set first (a texture without any reference left is on its way out: its slot stays empty)
    for (uint32_t i = 0; i <= s.n; i++) {
        vkn_tex **p = i < s.n ? &s.c[i] : &s.d;
        if (*p && !vkn_tex_ref(*p)) { g_st2.dead_target++; *p = NULL; }
    }
    for (uint32_t i = 0; i < g_cur.n; i++) vkn_tex_unref(g_cur.c[i]);
    vkn_tex_unref(g_cur.d);
    g_cur = s;
}

vkn_tex *vkn_pass_depth(void) { return g_cur.d; }
// ==== [stage 5.2 B1 + B2] the render-area hint of the UI piece (vkn_pass.h; the combination rule is in begin_set) ====
int vkn_pass_area_hint(const VkRect2D *area) {
    vkn_pass_init();
    if (!area) { g_hint_on = 0; return 0; }
    if (!area->extent.width || !area->extent.height) { g_hint_on = 0; return -1; }
    g_hint = *area; g_hint_on = 1; g_s3.hints++;
    return 0;
}
int vkn_pass_area_get(VkRect2D *out) {
    if (!(g_open && open_here() && same_set(&g_open_set, &g_cur))) return 0;
    if (out) *out = g_area;
    return 1;
}

int vkn_pass_ready(vkn_pass_info *info) {
    vkn_pass_init();
    if (!vkn_cb()) { g_st2.no_cb++; disarm(); return -1; }
    if (open_here() && same_set(&g_open_set, &g_cur)) {
        // [stage 5.2 B1] an area pass serves only draws the effect chain saw inside it (pass_fx); any other draw (the UI, a piece's
        // own, one whose facts went stale) ends it, and it begins again full with LOAD
        // ([B1 + B2] a hinted pass: the UI keeps its draws inside the area itself; engine draws pass pass_fx's check)
        if (!g_area_on || g_area_ok || g_area_src == 2) { g_area_ok = 0; disarm(); if (info) *info = g_info; return 0; }
        end_open(); g_s3.area_restart_other++;
    }
    g_area_ok = 0;
    const int r = begin_set(&g_cur);   // (uses the first-draw facts once)
    disarm();
    if (r) return -1;
    if (info) *info = g_info;
    return 0;
}

int vkn_pass_sample(vkn_tex *t) {
    vkn_pass_init();
    if (!t) return 0;
    WATCH(t, VKN_PASS_EV_SAMPLE, 0.0f);   // [stage 5 A3]
    if (in_set(&g_cur, t)) { g_st.feedback++; return 1; }
    if (!t->view) return 0;   // not sampled by the device (no SAMPLED usage): no layout for it here; the draw binds nothing of it
    if (!t->pass_nclr && !t->pass_dclr && t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        // [stage 5.2 B1] (no store-history event here: every write of the content leaves this layout, so the read that brought it back
        // to SHADER_READ_ONLY resolved what was stored; a read by other stages after a fragment-only barrier gets the full one)
        if (g_fragvis_n && !g_frag_only) fragvis_fix(t);
        return 0;
    }
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return 0; }
    if (g_open) { end_open(); g_st.breaks_sample++; }
    flush_clears(t);
    if (t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return 0;
    hist_read(t, RD_SAMPLE);   // [stage 5.2 B1]
    if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED && !zero_fill(cb, t)) g_st2.sampled_undefined++;   // (not a target: counted, content undefined)
    if (g_pol.cur.tilebar && g_frag_only) barrier_frag(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // [stage 5.2 B1] DM_VK_NATIVE_TILEBAR
    else { barrier(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); if (g_pol.cur.tilebar) g_s3.tilebar_full++; }
    return 0;
}

void vkn_pass_storage(vkn_tex *t) {
    vkn_pass_init();
    if (!t) return;
    WATCH(t, VKN_PASS_EV_USE, 0.0f);   // [stage 5 A3]
    disarm();   // [stage 5.2 B1]
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return; }
    end_open();
    flush_clears(t);
    hist_read(t, RD_STORAGE);   // [stage 5.2 B1]
    if (t->layout != VK_IMAGE_LAYOUT_GENERAL) barrier(cb, t, VK_IMAGE_LAYOUT_GENERAL);
    written(t);   // (a storage image is written by the dispatch, as far as this module knows)
}

void vkn_pass_layout(vkn_tex *t, VkImageLayout to) {
    vkn_pass_init();
    if (!t) return;
    WATCH(t, VKN_PASS_EV_USE, 0.0f);   // [stage 5 A3]
    disarm();   // [stage 5.2 B1]
    if (!t->pass_nclr && !t->pass_dclr && t->layout == to) { if (g_fragvis_n && to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) fragvis_fix(t); return; }
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return; }
    end_open();   // (any barrier ends the pass, not only one on its attachments)
    flush_clears(t);
    // [stage 5.2 B1] a transfer / present / read-back / sampling layout reads (or partly rewrites) the content; GENERAL is the sentinel of
    // vkn_post.c (its later real use comes through this module again)
    if (to != VK_IMAGE_LAYOUT_GENERAL) hist_read(t, RD_LAYOUT);
    if (t->layout != to) barrier(cb, t, to);
}

// ---- clears ----
void vkn_pass_clear_color(vkn_tex *t, uint32_t layer, const float rgba[4], const int32_t rect[4]) {
    vkn_pass_init();
    disarm();   // [stage 5.2 B1]
    if (g_dump.active && t && t->format == VK_FORMAT_R8G8B8A8_UNORM && t->w >= 1000) vkn_logf("trace: clear colour of %llx layer %u rect %d %d %d %d (%g %g %g %g), pass open %d", (unsigned long long)(t->handle & 0xffff), layer, rect ? rect[0] : 0, rect ? rect[1] : 0, rect ? rect[2] : -1, rect ? rect[3] : -1, rgba ? rgba[0] : 0, rgba ? rgba[1] : 0, rgba ? rgba[2] : 0, rgba ? rgba[3] : 0, open_here());
    if (!t || !rgba || is_depth(t) || layer >= t->layers) {
        g_st2.clear_bad++;
        if (red_ok(&g_red_clear)) vkn_logf(RED "pass: colour clear dropped (texture %#llx, layer %u of %u, depth format %d)", t ? (unsigned long long)t->handle : 0ull, layer, t ? t->layers : 0u, t ? is_depth(t) : 0);
        return;
    }
    // the region inside level 0 (0x928ab0 clears level 0 whole; the region clear of opcode 0x04 a part of it)
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)t->w, y1 = (int32_t)t->h;
    if (rect) {
        if (rect[0] > x0) x0 = rect[0]; if (rect[1] > y0) y0 = rect[1];
        if (rect[0] + rect[2] < x1) x1 = rect[0] + rect[2]; if (rect[1] + rect[3] < y1) y1 = rect[1] + rect[3];
        if (x1 <= x0 || y1 <= y0) return;   // nothing inside the texture (NVN clears nothing)
    }
    const int full = x0 == 0 && y0 == 0 && x1 == (int32_t)t->w && y1 == (int32_t)t->h;
    int slot = open_here() ? color_slot(&g_open_set, t, 0, layer) : -1;
    if (slot >= 0 && g_area_on && (x0 < g_area.offset.x || y0 < g_area.offset.y || x1 > g_area.offset.x + (int32_t)g_area.extent.width ||
                                   y1 > g_area.offset.y + (int32_t)g_area.extent.height)) { end_open(); slot = -1; g_s3.area_restart_clear++; g_hint_on = 0; }   // [stage 5.2 B1] outside the area (a hint is dropped: its promise is broken, the UI drops it too)
    if (slot < 0 && !full && color_slot(&g_cur, t, 0, layer) >= 0 && vkn_cb()) {   // a region of a current target: its pass serves
        if (!vkn_pass_ready(NULL)) slot = color_slot(&g_open_set, t, 0, layer);
    }
    if (slot < 0 && full) { clr_put(t, layer, rgba); return; }   // folded into the next pass, or run before the next other use
    if (slot < 0) {   // a region of a texture that is no attachment now: a one-attachment pass of its own (LOAD, the clear, end)
        tset one; memset(&one, 0, sizeof one);
        one.n = 1; one.c[0] = t; one.layer[0] = layer;
        if (begin_set(&one)) { g_st2.clear_lost++; return; }
        g_st2.region_pass++;
        slot = 0;
    }
    // inside the open pass: vkCmdClearAttachments over the part inside the render area
    const VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)slot, .clearValue.color = clear_color(t->format, rgba)};
    if (x0 < g_area.offset.x) x0 = g_area.offset.x;
    if (y0 < g_area.offset.y) y0 = g_area.offset.y;
    if (x1 > g_area.offset.x + (int32_t)g_area.extent.width) x1 = g_area.offset.x + (int32_t)g_area.extent.width;
    if (y1 > g_area.offset.y + (int32_t)g_area.extent.height) y1 = g_area.offset.y + (int32_t)g_area.extent.height;
    if (x1 > x0 && y1 > y0) {
        const VkClearRect cr = {{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, 1};
        vkCmdClearAttachments(g_open_cb, 1, &ca, 1, &cr);
        vkn_drv_cleared();   // [stage 5.3 V2] DM_GFX_CLEAR_REBIND: the bound vertex / index buffers may be disturbed on this driver
        g_st.clears_att++;
    }
    written(t);
    if (!same_set(&g_open_set, &g_cur)) end_open();   // (the one-attachment pass)
}

void vkn_pass_clear_depth(vkn_tex *t, int what, float depth, uint32_t stencil) {
    vkn_pass_init();
    disarm();   // [stage 5.2 B1]
    if (!t || !is_depth(t)) {
        g_st2.clear_bad++;
        if (red_ok(&g_red_clear)) vkn_logf(RED "pass: depth clear dropped (texture %#llx is %s)", t ? (unsigned long long)t->handle : 0ull, t ? "a colour texture" : "unknown");
        return;
    }
    VkImageAspectFlags a = 0;
    if ((what & 1) && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((what & 2) && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!a) return;
    if (a & VK_IMAGE_ASPECT_DEPTH_BIT) WATCH(t, VKN_PASS_EV_CLEAR, depth);   // [stage 5 A3]
    if (open_here() && g_open_set.d == t && g_area_on) { end_open(); g_s3.area_restart_clear++; }   // [stage 5.2 B1] (a whole-level clear: not inside an area pass)
    if (open_here() && g_open_set.d == t) {
        const VkClearAttachment ca = {.aspectMask = a, .clearValue.depthStencil = {depth, stencil}};
        const VkClearRect cr = {g_area, 0, 1};
        vkCmdClearAttachments(g_open_cb, 1, &ca, 1, &cr);
        vkn_drv_cleared();   // [stage 5.3 V2] DM_GFX_CLEAR_REBIND: the bound vertex / index buffers may be disturbed on this driver
        g_st.clears_att++;
        written(t);
        return;
    }
    if (a & VK_IMAGE_ASPECT_DEPTH_BIT) { t->pass_dclr |= 1; t->pass_depth = depth; }
    if (a & VK_IMAGE_ASPECT_STENCIL_BIT) { t->pass_dclr |= 2; t->pass_stencil = stencil; }
    t->pass_clr_ns = vkn_now_ns();
}

// ---- texture copy (0x920b00) ----
static uint32_t nvn_depth(const vkn_tex *t) { return t->nvn_target == 2 ? t->depth : t->layers; }   // NVN's "depth" of a texture: slices or layers
static int blit_ok(VkFormat f) {
    const vkn_dev *d = vkn_device(); if (!d) return 0;
    VkFormatProperties p; memset(&p, 0, sizeof p);
    vkGetPhysicalDeviceFormatProperties(d->phys, f, &p);
    const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    return (p.optimalTilingFeatures & want) == want;
}
void vkn_pass_copy(vkn_tex *dst, vkn_tex *src) {
    vkn_pass_init();
    disarm();   // [stage 5.2 B1]
    if (g_dump.active && dst && src) { dump_note(src); dump_note(dst); }
    if (g_dump.active && dst && src) vkn_logf("trace: copy %llx (%ux%u) -> %llx (%ux%u)", (unsigned long long)(src->handle & 0xffff), src->w, src->h, (unsigned long long)(dst->handle & 0xffff), dst->w, dst->h);
    if (!dst || !src || dst->dead || src->dead) {
        g_st2.copy_drop++;
        if (red_ok(&g_red_copy)) vkn_logf(RED "pass: copy dropped: %s texture %s", !dst || (dst && dst->dead) ? "destination" : "source", (!dst || !src) ? "unknown" : "destroyed");
        return;
    }
    if (dst->nvn_fmt != src->nvn_fmt) { g_st2.copy_skip_fmt++; return; }   // the engine copies nothing between different formats
    WATCH(dst, VKN_PASS_EV_USE, 0.0f); WATCH(src, VKN_PASS_EV_USE, 0.0f);   // [stage 5 A3]
    const int d3 = src->nvn_target == 2;
    if (dst->format != src->format || dst->samples != src->samples || dst == src || d3 != (dst->nvn_target == 2) || dst->aspect != src->aspect) {
        g_st2.copy_drop++;
        if (red_ok(&g_red_copy)) vkn_logf(RED "pass: copy %#llx -> %#llx dropped: formats %d / %d, samples %u / %u, targets %u / %u", (unsigned long long)src->handle,
                                         (unsigned long long)dst->handle, (int)src->format, (int)dst->format, src->samples, dst->samples, src->nvn_target, dst->nvn_target);
        return;
    }
    if (src->mips != 1 || dst->mips != 1) {
        if (src->mips < dst->mips) {   // the engine would read levels below 0 (unsigned wrap): nothing sane to copy
            g_st2.copy_drop++;
            if (red_ok(&g_red_copy)) vkn_logf(RED "pass: copy %#llx -> %#llx dropped: destination has more levels (%u) than the source (%u)", (unsigned long long)src->handle, (unsigned long long)dst->handle, dst->mips, src->mips);
            return;
        }
    }
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return; }
    // regions: one level each = one region of min(depth); else dst level i <- src level (src levels - dst levels + i), each level's
    // size and depth (NVN depth shifted by the level, at least 1: the engine's own computation, arrays included)
    VkImageCopy cp[16]; VkImageBlit bl[16]; uint32_t ncp = 0, nbl = 0;
    const uint32_t nl = src->mips == 1 && dst->mips == 1 ? 1 : (dst->mips < 16 ? dst->mips : 16);
    for (uint32_t i = 0; i < nl; i++) {
        const uint32_t sl = src->mips == 1 && dst->mips == 1 ? 0 : src->mips - dst->mips + i, dl = i;
        const uint32_t sw = mipdim(src->w, sl), sh = mipdim(src->h, sl), dw = mipdim(dst->w, dl), dh = mipdim(dst->h, dl);
        uint32_t sd = nvn_depth(src), dd = nvn_depth(dst);
        if (nl > 1 || src->mips != 1 || dst->mips != 1) { sd = mipdim(sd, sl); dd = mipdim(dd, dl); }
        const uint32_t n = sd < dd ? sd : dd;
        const VkImageSubresourceLayers ss = {src->aspect, sl, 0, d3 ? 1 : n}, ds = {dst->aspect, dl, 0, d3 ? 1 : n};
        if (sw == dw && sh == dh) cp[ncp++] = (VkImageCopy){ss, {0, 0, 0}, ds, {0, 0, 0}, {sw, sh, d3 ? n : 1}};
        else bl[nbl++] = (VkImageBlit){ss, {{0, 0, 0}, {(int32_t)sw, (int32_t)sh, d3 ? (int32_t)n : 1}}, ds, {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, d3 ? (int32_t)n : 1}}};
    }
    if (nbl && !blit_ok(src->format)) {
        g_st2.copy_drop++;
        if (red_ok(&g_red_copy)) vkn_logf(RED "pass: scaled copy %#llx (%ux%u) -> %#llx (%ux%u): format %d cannot be blitted", (unsigned long long)src->handle, src->w, src->h,
                                         (unsigned long long)dst->handle, dst->w, dst->h, (int)src->format);
        nbl = 0;
        if (!ncp) return;
    }
    end_open();
    // A creation upload still queued for one of them (the texture was made after this recording began) would be recorded at the
    // NEXT recording's start and overwrite what is copied here (phone run V4_a: save thumbnails all zero - the engine makes its
    // read-back texture with initial data, copies the screenshot into it, and the read-back's own recording then began with that
    // queued upload). So queued uploads run now, in order.
    if (vkn_upload_pending_bytes()) (void)vkn_upload_run(cb);
    flush_clears(src); flush_clears(dst);
    // [stage 5.2 B1] the source's content is read; a one-subresource destination is overwritten whole (one region of its full size)
    hist_read(src, RD_COPY);
    if (dst->mips == 1 && dst->layers == 1 && dst->nvn_target != 2 && ncp + nbl == 1) hist_over(dst);
    if (src->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (dst->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (ncp) vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ncp, cp);
    if (nbl) { vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nbl, bl, VK_FILTER_NEAREST); g_st2.copy_blit++; }
    written(dst);
    g_st.copies++;
}

void vkn_pass_stats_get(vkn_pass_stats *s) { if (s) *s = g_st; }
void vkn_pass_stats2_get(vkn_pass_stats2 *s) { if (s) *s = g_st2; }
// ==== [stage 5 A3] the shadow cache (vkn_pass.h) ====
void vkn_pass_cur_get(uint32_t *n, vkn_tex *color[VKN_PASS_COLORS], uint32_t layer[VKN_PASS_COLORS], uint32_t mip[VKN_PASS_COLORS], vkn_tex **depth) {
    if (n) *n = g_cur.n;
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) {
        if (color) color[i] = i < g_cur.n ? g_cur.c[i] : NULL;
        if (layer) layer[i] = i < g_cur.n ? g_cur.layer[i] : 0;
        if (mip) mip[i] = i < g_cur.n ? g_cur.mip[i] : 0;
    }
    if (depth) *depth = g_cur.d;
}
void vkn_pass_watch(vkn_tex *t, vkn_pass_watch_fn fn) { g_watch_t = fn ? t : NULL; g_watch_fn = t ? fn : NULL; }
#endif
