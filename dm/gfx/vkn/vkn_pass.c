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
// Barriers are conservative (ALL_COMMANDS, memory read / write): the tile-GPU refinement is stage 4, like DONT_CARE for covered targets.
#ifdef DM_GAME_DOS2
#include "vkn_pass.h"
#include "vkn_frame.h"
#include "vkn_state.h"
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
static uint32_t mipdim(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }
static int has_content(const vkn_tex *t) { return t->pass_ever || t->layout != VK_IMAGE_LAYOUT_UNDEFINED; }   // (uploaded data counts)
static int is_depth(const vkn_tex *t) { return (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0; }
static void written(vkn_tex *t) { t->pass_ever = 1; t->pass_written = vkn_frame() + 1; }

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
}

static void end_open(void) {
    if (g_open && open_here()) vkCmdEndRenderPass(g_open_cb);
    g_open = 0; g_open_cb = VK_NULL_HANDLE;
}
static void on_begin(void) { g_open = 0; g_open_cb = VK_NULL_HANDLE; }   // a new recording: nothing of the last one is open

void vkn_pass_init(void) {
    if (g_inited) return;
    g_inited = 1;
    vkn_rec_on_close(vkn_pass_end);
    vkn_rec_on_begin(on_begin);
}
void vkn_pass_end(void) { end_open(); }

// Executes the remembered clears of t as commands of their own (outside any pass). 0 = done or nothing to do; -1 = no recording
// (they stay remembered).
static int flush_clears(vkn_tex *t) {
    if (!t->pass_nclr && !t->pass_dclr) return 0;
    VkCommandBuffer cb = vkn_cb();
    if (!cb) return -1;
    end_open();
    barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
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
    t->pass_nclr = 0; t->pass_dclr = 0;
    written(t);
    return 0;
}

// Remembers a full clear of (level 0, layer) of t (the last one of a layer wins).
static void clr_put(vkn_tex *t, uint32_t layer, const float rgba[4]) {
    for (uint32_t i = 0; i < t->pass_nclr; i++)
        if (t->pass_clr[i].layer == layer) { memcpy(t->pass_clr[i].rgba, rgba, sizeof t->pass_clr[i].rgba); return; }
    if (t->pass_nclr >= 4 && flush_clears(t)) {   // more layers than the record holds and no recording to run them: the oldest is lost
        g_st2.clear_lost++;
        memmove(&t->pass_clr[0], &t->pass_clr[1], sizeof t->pass_clr[0] * 3); t->pass_nclr = 3;
    }
    t->pass_clr[t->pass_nclr].layer = layer; memcpy(t->pass_clr[t->pass_nclr].rgba, rgba, sizeof t->pass_clr[0].rgba); t->pass_nclr++;
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
static int begin_set(const tset *s) {
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return -1; }
    end_open();
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
    if (bad) return -1;
    if (!any) { g_st2.no_targets++; return -1; }
    if (wmin) { g_st2.size_mismatch++; if (red_ok(&g_red_size)) vkn_logf(RED "pass: attachments of different sizes: rendering into %ux%u (the smallest; widest %u)", W, H, wmax); }
    // load / store ops (the rule of vkn_pass.h) and the layouts, before the pass begins
    nv = 0;
    for (uint32_t i = 0; i < s->n; i++) {
        vkn_tex *t = s->c[i];
        if (!t) continue;
        float rgba[4];
        if (s->mip[i] == 0 && clr_find(t, s->layer[i], rgba, 0)) { k.load[i] = VK_ATTACHMENT_LOAD_OP_CLEAR; cv[nv].color = clear_color(t->format, rgba); g_st.clear++; g_st.clears_folded++; }
        else if (has_content(t)) { k.load[i] = VK_ATTACHMENT_LOAD_OP_LOAD; g_st.load++; if (t->pass_written != vkn_frame() + 1) g_st2.load_stale++; }
        else { k.load[i] = VK_ATTACHMENT_LOAD_OP_DONT_CARE; g_st.dontcare++; }
        k.store[i] = VK_ATTACHMENT_STORE_OP_STORE;
        k.color[i] = (uint32_t)t->format;
        inf.write_mask[i] = vkn_write_mask_nvn(t->nvn_fmt);
        if (t->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) barrier(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        nv++;
    }
    k.ncolor = (uint8_t)ncolor;
    k.samples = (uint8_t)(samples ? samples : 1);
    if (s->d) {
        vkn_tex *t = s->d;
        const int hd = (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, hs = (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0, content = has_content(t);
        k.depth = (uint32_t)t->format;
        k.depth_load = (t->pass_dclr & 1) && hd ? VK_ATTACHMENT_LOAD_OP_CLEAR : content ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        k.stencil_load = (t->pass_dclr & 2) && hs ? VK_ATTACHMENT_LOAD_OP_CLEAR : content ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        k.depth_store = k.stencil_store = VK_ATTACHMENT_STORE_OP_STORE;
        if (k.depth_load == VK_ATTACHMENT_LOAD_OP_CLEAR || k.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            cv[nv].depthStencil.depth = t->pass_depth; cv[nv].depthStencil.stencil = t->pass_stencil;
            g_st.clear++; g_st.clears_folded++;
        } else if (content) { g_st.load++; if (t->pass_written != vkn_frame() + 1) g_st2.load_stale++; }
        else g_st.dontcare++;
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
                                      .renderArea = {{0, 0}, {W, H}}, .clearValueCount = nv, .pClearValues = cv};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g_open = 1; g_open_cb = cb; g_open_serial = vkn_cb_serial(); g_open_set = *s;
    g_area = bi.renderArea;
    // the remembered clears this pass performs leave the records only now (a pass that failed above keeps them)
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i] && k.load[i] == VK_ATTACHMENT_LOAD_OP_CLEAR) clr_find(s->c[i], s->layer[i], NULL, 1);
    if (s->d) s->d->pass_dclr = 0;   // (an aspect the format lacks has nothing to clear)
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i]) written(s->c[i]);
    if (s->d) written(s->d);
    inf.pass = rp; inf.compat = compat; inf.ncolor = ncolor; inf.width = W; inf.height = H; inf.samples = k.samples;
    g_info = inf;
    g_st.passes++;
    return 0;
}

void vkn_pass_targets(uint32_t n, vkn_tex *const color[VKN_PASS_COLORS], const uint32_t layer[VKN_PASS_COLORS], const uint32_t mip[VKN_PASS_COLORS], vkn_tex *depth) {
    vkn_pass_init();
    tset s; memset(&s, 0, sizeof s);
    s.n = n > VKN_PASS_COLORS ? VKN_PASS_COLORS : n;
    for (uint32_t i = 0; i < s.n; i++) {
        s.c[i] = color ? color[i] : NULL;
        if (s.c[i]) { s.layer[i] = layer ? layer[i] : 0; s.mip[i] = mip ? mip[i] : 0; }
    }
    s.d = depth;
    if (same_set(&s, &g_cur)) return;   // the open pass (if any) stays open
    end_open();
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

int vkn_pass_ready(vkn_pass_info *info) {
    vkn_pass_init();
    if (!vkn_cb()) { g_st2.no_cb++; return -1; }
    if (open_here() && same_set(&g_open_set, &g_cur)) { if (info) *info = g_info; return 0; }
    if (begin_set(&g_cur)) return -1;
    if (info) *info = g_info;
    return 0;
}

int vkn_pass_sample(vkn_tex *t) {
    vkn_pass_init();
    if (!t) return 0;
    if (in_set(&g_cur, t)) { g_st.feedback++; return 1; }
    if (!t->view) return 0;   // not sampled by the device (no SAMPLED usage): no layout for it here; the draw binds nothing of it
    if (!t->pass_nclr && !t->pass_dclr && t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return 0;
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return 0; }
    if (g_open) { end_open(); g_st.breaks_sample++; }
    flush_clears(t);
    if (t->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return 0;
    if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED) g_st2.sampled_undefined++;   // never written, never uploaded: its content is undefined (as on NVN)
    barrier(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 0;
}

void vkn_pass_storage(vkn_tex *t) {
    vkn_pass_init();
    if (!t) return;
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return; }
    end_open();
    flush_clears(t);
    if (t->layout != VK_IMAGE_LAYOUT_GENERAL) barrier(cb, t, VK_IMAGE_LAYOUT_GENERAL);
    written(t);   // (a storage image is written by the dispatch, as far as this module knows)
}

void vkn_pass_layout(vkn_tex *t, VkImageLayout to) {
    vkn_pass_init();
    if (!t) return;
    if (!t->pass_nclr && !t->pass_dclr && t->layout == to) return;
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st2.no_cb++; return; }
    end_open();   // (any barrier ends the pass, not only one on its attachments)
    flush_clears(t);
    if (t->layout != to) barrier(cb, t, to);
}

// ---- clears ----
void vkn_pass_clear_color(vkn_tex *t, uint32_t layer, const float rgba[4], const int32_t rect[4]) {
    vkn_pass_init();
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
    if (x1 > (int32_t)g_area.extent.width) x1 = (int32_t)g_area.extent.width;
    if (y1 > (int32_t)g_area.extent.height) y1 = (int32_t)g_area.extent.height;
    if (x1 > x0 && y1 > y0) {
        const VkClearRect cr = {{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, 1};
        vkCmdClearAttachments(g_open_cb, 1, &ca, 1, &cr);
        g_st.clears_att++;
    }
    written(t);
    if (!same_set(&g_open_set, &g_cur)) end_open();   // (the one-attachment pass)
}

void vkn_pass_clear_depth(vkn_tex *t, int what, float depth, uint32_t stencil) {
    vkn_pass_init();
    if (!t || !is_depth(t)) {
        g_st2.clear_bad++;
        if (red_ok(&g_red_clear)) vkn_logf(RED "pass: depth clear dropped (texture %#llx is %s)", t ? (unsigned long long)t->handle : 0ull, t ? "a colour texture" : "unknown");
        return;
    }
    VkImageAspectFlags a = 0;
    if ((what & 1) && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((what & 2) && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!a) return;
    if (open_here() && g_open_set.d == t) {
        const VkClearAttachment ca = {.aspectMask = a, .clearValue.depthStencil = {depth, stencil}};
        const VkClearRect cr = {g_area, 0, 1};
        vkCmdClearAttachments(g_open_cb, 1, &ca, 1, &cr);
        g_st.clears_att++;
        written(t);
        return;
    }
    if (a & VK_IMAGE_ASPECT_DEPTH_BIT) { t->pass_dclr |= 1; t->pass_depth = depth; }
    if (a & VK_IMAGE_ASPECT_STENCIL_BIT) { t->pass_dclr |= 2; t->pass_stencil = stencil; }
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
    if (!dst || !src || dst->dead || src->dead) {
        g_st2.copy_drop++;
        if (red_ok(&g_red_copy)) vkn_logf(RED "pass: copy dropped: %s texture %s", !dst || (dst && dst->dead) ? "destination" : "source", (!dst || !src) ? "unknown" : "destroyed");
        return;
    }
    if (dst->nvn_fmt != src->nvn_fmt) { g_st2.copy_skip_fmt++; return; }   // the engine copies nothing between different formats
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
    flush_clears(src); flush_clears(dst);
    if (src->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (dst->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (ncp) vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ncp, cp);
    if (nbl) { vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nbl, bl, VK_FILTER_NEAREST); g_st2.copy_blit++; }
    written(dst);
    g_st.copies++;
}

void vkn_pass_stats_get(vkn_pass_stats *s) { if (s) *s = g_st; }
void vkn_pass_stats2_get(vkn_pass_stats2 *s) { if (s) *s = g_st2; }
#endif
