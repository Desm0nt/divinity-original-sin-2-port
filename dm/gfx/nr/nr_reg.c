// [DOS2] NR-01: the engine object registry (see nr_reg.h). Pure C, host-testable (dm/gfx/vulkan/engine/tests/nr_reg_test.cpp).
#include "nr_reg.h"
#include <stdlib.h>
#include <string.h>

// Engine format enum 0..62: name, NVN format, NVN format with the sRGB bit, channels, bytes per pixel (per 4x4 block when compressed),
// compressed. Verified against main.raw by tools/dos2_nr_check.py (0x919c20 jump table 0x1f5051c, 0x91eea0 jump table 0x1f5058c).
const nr_fmt_row nr_formats[NR_FMT_N] = {
    {"None", 0x0, 0x0, 0, 0, 0},
    {"R8", 0x1, 0x1, 1, 1, 0},
    {"RG8", 0xd, 0xd, 2, 2, 0},
    {"RGB8", 0x19, 0x19, 3, 3, 0},
    {"RGBA8", 0x25, 0x38, 4, 4, 0},
    {"R16", 0x6, 0x6, 1, 2, 0},
    {"RG16", 0x12, 0x12, 2, 4, 0},
    {"RGB16", 0x1e, 0x1e, 3, 6, 0},
    {"RGBA16", 0x2a, 0x2a, 4, 8, 0},
    {"R8S", 0x2, 0x2, 1, 1, 0},
    {"RG8S", 0xe, 0xe, 2, 2, 0},
    {"RGB8S", 0x1a, 0x1a, 3, 3, 0},
    {"RGBA8S", 0x26, 0x26, 4, 4, 0},
    {"R16S", 0x7, 0x7, 1, 2, 0},
    {"RG16S", 0x13, 0x13, 2, 4, 0},
    {"RGB16S", 0x1f, 0x1f, 3, 6, 0},
    {"RGBA16S", 0x2b, 0x2b, 4, 8, 0},
    {"R16F", 0x5, 0x5, 1, 2, 0},
    {"RG16F", 0x11, 0x11, 2, 4, 0},
    {"RGB16F", 0x1d, 0x1d, 3, 6, 0},
    {"RGBA16F", 0x29, 0x29, 4, 8, 0},
    {"R32F", 0xa, 0xa, 1, 4, 0},
    {"RG32F", 0x16, 0x16, 2, 8, 0},
    {"RGB32F", 0x22, 0x22, 3, 12, 0},
    {"RGBA32F", 0x2e, 0x2e, 4, 16, 0},
    {"R16I", 0x9, 0x9, 1, 2, 0},
    {"RG16I", 0x15, 0x15, 2, 4, 0},
    {"RGB16I", 0x21, 0x21, 3, 6, 0},
    {"RGBA16I", 0x2d, 0x2d, 4, 8, 0},
    {"R32I", 0xc, 0xc, 1, 4, 0},
    {"RG32I", 0x18, 0x18, 2, 8, 0},
    {"RGB32I", 0x24, 0x24, 3, 12, 0},
    {"RGBA32I", 0x30, 0x30, 4, 16, 0},
    {"R16UI", 0x8, 0x8, 1, 2, 0},
    {"RG16UI", 0x14, 0x14, 2, 4, 0},
    {"RGB16UI", 0x20, 0x20, 3, 6, 0},
    {"RGBA16UI", 0x2c, 0x2c, 4, 8, 0},
    {"R32UI", 0xb, 0xb, 1, 4, 0},
    {"RG32UI", 0x17, 0x17, 2, 8, 0},
    {"RGB32UI", 0x23, 0x23, 3, 12, 0},
    {"RGBA32UI", 0x2f, 0x2f, 4, 16, 0},
    {"RGBE8", 0x0, 0x0, 3, 4, 0},
    {"RGB9E5", 0x40, 0x40, 3, 4, 0},
    {"RG11B10F", 0x3f, 0x3f, 3, 4, 0},
    {"RGB565", 0x95, 0x95, 3, 2, 0},
    {"RGBA4", 0x39, 0x39, 4, 2, 0},
    {"RGB10A2", 0x3d, 0x3d, 4, 4, 0},
    {"BGR10A2", 0xffff, 0xffff, 4, 4, 0},
    {"A1RGB5", 0x98, 0x98, 4, 2, 0},
    {"D16", 0x32, 0x32, 1, 2, 0},
    {"D24", 0x33, 0x33, 1, 4, 0},
    {"D24S8", 0x35, 0x35, 2, 4, 0},
    {"D32F", 0x34, 0x34, 1, 4, 0},
    {"DXT1", 0x41, 0x45, 3, 8, 1},
    {"DXT3", 0x43, 0x47, 4, 16, 1},
    {"DXT5", 0x44, 0x48, 4, 16, 1},
    {"BC4U", 0x49, 0x49, 1, 8, 1},
    {"BC5U", 0x4b, 0x4b, 2, 16, 1},
    {"BC6H", 0x50, 0x50, 3, 16, 1},
    {"BC7", 0x4d, 0x4d, 4, 16, 1},
    {"BGR8", 0x99, 0x99, 3, 3, 0},
    {"BGRA8", 0x9a, 0x9c, 4, 4, 0},
    {"D32FS8", 0xffff, 0xffff, 1, 8, 0},
};

const char *nr_fmt_name(unsigned f) { return f < NR_FMT_N ? nr_formats[f].name : "?"; }
int nr_fmt_class(unsigned f) {
    if (!f || f >= NR_FMT_N) return NR_CLS_NONE;
    if (nr_formats[f].compressed) return NR_CLS_BC;
    if ((f >= 49 && f <= 52) || f == 62) return NR_CLS_DEPTH;
    if ((f >= 1 && f <= 4) || (f >= 9 && f <= 12) || (f >= 44 && f <= 48) || f == 60 || f == 61) return NR_CLS_U8;
    return NR_CLS_HDR;   // 16 / 32-bit, float, integer, shared exponent, 10-bit
}
const char *nr_cls_name(int c) { static const char *const n[NR_CLS_N] = {"none", "u8", "hdr", "depth", "bc"}; return c >= 0 && c < NR_CLS_N ? n[c] : "?"; }
const char *nr_src_name(int s) {
    static const char *const n[NR_SRC_N] = {"none", "create", "create-data", "image-linear", "image-packaged", "rt", "window"};
    return s >= 0 && s < NR_SRC_N ? n[s] : "?";
}
const char *nr_state_name(int k) { static const char *const n[NR_ST_N] = {"blend", "depth-stencil", "raster"}; return k >= 0 && k < NR_ST_N ? n[k] : "?"; }
const char *nr_kind_name(int k) {
    static const char *const n[NR_K_N] = {"texture", "shader", "sampler", "blend", "depth-stencil", "raster", "vformat", "vbuffer", "ibuffer"};
    return k >= 0 && k < NR_K_N ? n[k] : "?";
}
// CreateTextureFromImage (0x91f2e0) swaps the image's own NVN format inside these linear / sRGB pairs by the sRGB bit of the call.
static const uint16_t k_pair[][3] = {   // {case value, linear, sRGB}
    {0x25, 0x25, 0x38}, {0x41, 0x41, 0x45}, {0x45, 0x41, 0x45}, {0x42, 0x42, 0x46}, {0x46, 0x42, 0x46},
    {0x43, 0x43, 0x47}, {0x47, 0x43, 0x47}, {0x44, 0x44, 0x48}, {0x48, 0x44, 0x48}, {0x6d, 0x6d, 0x37}};
int nr_expect_nvn_format(unsigned efmt, int srgb, int ov, int from_image) {
    if (from_image ? ov != -1 : ov != 0) {
        if (from_image) for (size_t i = 0; i < sizeof k_pair / sizeof k_pair[0]; i++) if (k_pair[i][0] == ov) return srgb ? k_pair[i][2] : k_pair[i][1];
        return ov;
    }
    if (efmt >= NR_FMT_N) return -1;
    const int v = srgb ? nr_formats[efmt].nvn_srgb : nr_formats[efmt].nvn;
    return v == NR_NVN_TRAP ? -1 : v;
}

nr_reg *nr_reg_new(void) {
    nr_reg *r = (nr_reg *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->tex = (nr_tex *)calloc(NR_TEX_MAX, sizeof *r->tex); r->shader = (nr_shader *)calloc(NR_SHADER_MAX, sizeof *r->shader);
    r->sampler = (nr_sampler *)calloc(NR_SAMPLER_MAX, sizeof *r->sampler); r->vformat = (nr_vformat *)calloc(NR_VF_MAX, sizeof *r->vformat);
    for (int k = 0; k < NR_ST_N; k++) r->state[k] = (nr_state *)calloc(NR_STATE_MAX, sizeof *r->state[k]);
    for (int k = 0; k < NR_BUF_N; k++) r->buf[k] = (nr_buffer *)calloc(NR_BUF_MAX, sizeof *r->buf[k]);
    r->rtset = (nr_rtset *)calloc(NR_RTSET_MAX, sizeof *r->rtset); r->trans = (nr_trans *)calloc(NR_TRANS_MAX, sizeof *r->trans);
    r->pool = (nr_pool *)calloc(NR_POOL_MAX, sizeof *r->pool);
    int ok = r->tex && r->shader && r->sampler && r->vformat && r->rtset && r->trans && r->pool;
    for (int k = 0; k < NR_ST_N; k++) ok = ok && r->state[k];
    for (int k = 0; k < NR_BUF_N; k++) ok = ok && r->buf[k];
    if (!ok) { nr_reg_free(r); return NULL; }
    return r;
}
void nr_reg_free(nr_reg *r) {
    if (!r) return;
    free(r->tex); free(r->shader); free(r->sampler); free(r->vformat); free(r->rtset); free(r->trans); free(r->pool);
    for (int k = 0; k < NR_ST_N; k++) free(r->state[k]);
    for (int k = 0; k < NR_BUF_N; k++) free(r->buf[k]);
    free(r);
}

// The slot of `handle` in a table of `max` entries, or -1 (counted). Updates the high-water mark. need_gen: the engine checks the
// generation of this kind on every use (textures, shaders, buffers), so a handle without one is no handle; samplers, state objects
// and vertex formats are only ever indexed by the low word, so any value is stored.
static long nr_slot(nr_kind_stat *s, uint64_t handle, uint32_t max, int need_gen) {
    if (need_gen && !nr_h_gen(handle)) { s->bad_handle++; return -1; }
    const uint32_t i = nr_h_idx(handle);
    if (i >= max) { s->overflow++; return -1; }
    if (i >= s->hi) s->hi = i + 1;
    return (long)i;
}
static void nr_copy(char *dst, size_t cap, const char *src) {   // bounded, always terminated, control characters / tabs made '?' (TSV safe)
    size_t n = 0;
    if (src) for (; n + 1 < cap && src[n]; n++) dst[n] = (unsigned char)src[n] < 0x20 || src[n] == 0x7f ? '?' : src[n];
    dst[n] = 0;
}

nr_tex *nr_tex_put(nr_reg *r, uint64_t handle, const nr_tex_desc *e, const nr_nvn_tex *nvn, int nnvn) {
    nr_kind_stat *s = &r->st[NR_K_TEX];
    const long i = nr_slot(s, handle, NR_TEX_MAX, 1);
    if (i < 0) return NULL;
    nr_tex *t = &r->tex[i];
    const uint32_t creates = t->creates, destroys = t->destroys;
    uint32_t replaced = t->replaced;
    if (t->alive) { replaced++; s->replaced++; }
    memset(t, 0, sizeof *t);
    t->handle = handle; t->w = e->w; t->h = e->h; t->d = e->d; t->fmt_override = e->fmt_override; t->eflags = e->eflags; t->eflags2 = e->eflags2;
    t->efmt = e->efmt; t->mips = e->mips; t->layers = e->layers; t->msaa = e->msaa; t->src = e->src; t->srgb = e->srgb; t->alive = 1;
    t->nnvn = (uint8_t)(nnvn < 0 ? 0 : nnvn > 255 ? 255 : nnvn);
    if (nvn && nnvn > 0) t->nvn = *nvn;
    t->creates = creates + 1; t->destroys = destroys; t->replaced = replaced;
    s->puts++;
    return t;
}
int nr_tex_kill(nr_reg *r, uint64_t handle) {
    nr_kind_stat *s = &r->st[NR_K_TEX];
    const uint32_t i = nr_h_idx(handle);
    if (!nr_h_gen(handle) || i >= NR_TEX_MAX || !r->tex[i].alive || r->tex[i].handle != handle) { s->kill_miss++; return 0; }
    r->tex[i].alive = 0; r->tex[i].destroys++; s->kills++;
    return 1;
}
int nr_tex_kill_obj(nr_reg *r, uint64_t obj) {
    nr_kind_stat *s = &r->st[NR_K_TEX];
    if (obj) for (uint32_t i = 0; i < s->hi; i++) if (r->tex[i].alive && r->tex[i].nvn.obj == obj) { r->tex[i].alive = 0; r->tex[i].destroys++; s->kills++; return 1; }
    s->kill_miss++;
    return 0;
}
const nr_tex *nr_tex_get(const nr_reg *r, uint64_t handle) {
    const uint32_t i = nr_h_idx(handle);
    return nr_h_gen(handle) && i < NR_TEX_MAX && r->tex[i].alive && r->tex[i].handle == handle ? &r->tex[i] : NULL;
}
nr_sampler *nr_sampler_put(nr_reg *r, uint64_t handle, unsigned filter, unsigned wu, unsigned wv, unsigned ww, unsigned aniso, unsigned compare, const nr_nvn_smp *nvn) {
    nr_kind_stat *s = &r->st[NR_K_SAMPLER];
    const long i = nr_slot(s, handle, NR_SAMPLER_MAX, 0);
    if (i < 0) return NULL;
    nr_sampler *o = &r->sampler[i];
    const uint32_t creates = o->creates;
    if (o->alive) s->replaced++;
    memset(o, 0, sizeof *o);
    o->handle = handle; o->filter = (uint8_t)filter; o->wrap[0] = (uint8_t)wu; o->wrap[1] = (uint8_t)wv; o->wrap[2] = (uint8_t)ww;
    o->aniso = (uint8_t)(aniso > 255 ? 255 : aniso); o->compare = (uint8_t)compare; o->alive = 1; o->creates = creates + 1;
    if (nvn) { o->nvn = *nvn; o->nvn.have = 1; }
    s->puts++;
    return o;
}
nr_state *nr_state_put(nr_reg *r, int kind, uint64_t handle, const uint8_t *arg, int nargs) {
    if (kind < 0 || kind >= NR_ST_N) return NULL;
    nr_kind_stat *s = &r->st[NR_K_BLEND + kind];
    const long i = nr_slot(s, handle, NR_STATE_MAX, 0);
    if (i < 0) return NULL;
    nr_state *o = &r->state[kind][i];
    const uint32_t creates = o->creates;
    if (o->alive) s->replaced++;
    memset(o, 0, sizeof *o);
    o->handle = handle; o->nargs = (uint8_t)(nargs < 0 ? 0 : nargs > NR_ST_ARGS ? NR_ST_ARGS : nargs); o->alive = 1; o->creates = creates + 1;
    if (arg) memcpy(o->arg, arg, o->nargs);
    s->puts++;
    return o;
}
nr_vformat *nr_vformat_put(nr_reg *r, uint64_t handle, const uint8_t *elems, unsigned count) {
    nr_kind_stat *s = &r->st[NR_K_VFORMAT];
    const long i = nr_slot(s, handle, NR_VF_MAX, 0);
    if (i < 0) return NULL;
    nr_vformat *o = &r->vformat[i];
    if (o->alive && o->handle == handle) { o->creates++; s->puts++; return o; }   // the engine found an equal format: same handle again
    const uint32_t creates = o->creates;
    if (o->alive) s->replaced++;
    memset(o, 0, sizeof *o);
    o->handle = handle; o->count = (uint16_t)(count > 0xffff ? 0xffff : count); o->kept = (uint8_t)(count > NR_VF_ELEMS ? NR_VF_ELEMS : count);
    o->alive = 1; o->creates = creates + 1;
    if (elems) memcpy(o->elem, elems, (size_t)o->kept * 6); else o->kept = 0;
    s->puts++;
    return o;
}
nr_buffer *nr_buffer_put(nr_reg *r, int kind, uint64_t handle, uint64_t size, uint32_t count, unsigned index_size, unsigned usage, uint64_t nvn_obj, uint64_t nvn_size) {
    if (kind < 0 || kind >= NR_BUF_N) return NULL;
    nr_kind_stat *s = &r->st[NR_K_VB + kind];
    const long i = nr_slot(s, handle, NR_BUF_MAX, 1);
    if (i < 0) return NULL;
    nr_buffer *o = &r->buf[kind][i];
    const uint32_t creates = o->creates, destroys = o->destroys;
    uint32_t replaced = o->replaced;
    if (o->alive) { replaced++; s->replaced++; }
    memset(o, 0, sizeof *o);
    o->handle = handle; o->size = size; o->count = count; o->index_size = (uint8_t)index_size; o->usage = (uint8_t)usage; o->nvn_obj = nvn_obj; o->nvn_size = nvn_size;
    o->alive = 1; o->creates = creates + 1; o->destroys = destroys; o->replaced = replaced;
    s->puts++;
    return o;
}
int nr_buffer_kill(nr_reg *r, int kind, uint64_t handle) {
    if (kind < 0 || kind >= NR_BUF_N) return 0;
    nr_kind_stat *s = &r->st[NR_K_VB + kind];
    const uint32_t i = nr_h_idx(handle);
    if (!nr_h_gen(handle) || i >= NR_BUF_MAX || !r->buf[kind][i].alive || r->buf[kind][i].handle != handle) { s->kill_miss++; return 0; }
    r->buf[kind][i].alive = 0; r->buf[kind][i].destroys++; s->kills++;
    return 1;
}
nr_shader *nr_shader_put(nr_reg *r, uint64_t handle, const char *name, const char *path, const uint64_t prog[NR_STAGES], const char key[NR_STAGES][17]) {
    nr_kind_stat *s = &r->st[NR_K_SHADER];
    const long i = nr_slot(s, handle, NR_SHADER_MAX, 1);
    if (i < 0) return NULL;
    nr_shader *o = &r->shader[i];
    const uint32_t creates = o->creates;
    uint32_t replaced = o->replaced;
    if (o->alive) { replaced++; s->replaced++; }
    memset(o, 0, sizeof *o);
    o->handle = handle; o->alive = 1; o->creates = creates + 1; o->replaced = replaced;
    nr_copy(o->name, sizeof o->name, name); nr_copy(o->path, sizeof o->path, path);
    for (int k = 0; k < NR_STAGES; k++) { if (prog) o->prog[k] = prog[k]; if (key) nr_copy(o->key[k], sizeof o->key[k], key[k]); }
    s->puts++;
    return o;
}
const nr_shader *nr_shader_get(const nr_reg *r, uint64_t handle) {
    const uint32_t i = nr_h_idx(handle);
    return nr_h_gen(handle) && i < NR_SHADER_MAX && r->shader[i].alive && r->shader[i].handle == handle ? &r->shader[i] : NULL;
}
int nr_shader_note_get(nr_reg *r, uint64_t value, const char *name) {
    r->shader_gets++;
    const uint32_t i = nr_h_idx(value);
    if (!nr_h_gen(value) || i >= NR_SHADER_MAX || !r->shader[i].alive || r->shader[i].handle != value) { r->shader_get_miss++; return 0; }
    nr_shader *o = &r->shader[i];
    o->gets++;
    if (!o->name[0] && name) nr_copy(o->name, sizeof o->name, name);
    return 1;
}

static uint64_t nr_mix(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001b3ull; return h ^ (h >> 29); }
int nr_rtset_note(nr_reg *r, unsigned n, const uint64_t *color, uint64_t depth) {
    r->rtset_calls++;
    if (n > NR_RT_COLORS) { r->rtset_lost++; return -1; }
    uint64_t h = nr_mix(0xcbf29ce484222325ull, n); h = nr_mix(h, depth);
    for (unsigned i = 0; i < n; i++) h = nr_mix(h, color ? color[i] : 0);
    for (unsigned p = (unsigned)h & (NR_RTSET_MAX - 1), k = 0; k < NR_RTSET_MAX; p = (p + 1) & (NR_RTSET_MAX - 1), k++) {
        nr_rtset *s = &r->rtset[p];
        if (!s->seq) {
            if (r->nrtset >= NR_RTSET_MAX * 3 / 4) break;
            s->n = n; s->depth = depth; s->count = 1; s->seq = ++r->nrtset;
            for (unsigned i = 0; i < n; i++) s->color[i] = color ? color[i] : 0;
            return 1;
        }
        if (s->n == n && s->depth == depth) {
            unsigned i = 0;
            while (i < n && s->color[i] == (color ? color[i] : 0)) i++;
            if (i == n) { s->count++; return 0; }
        }
    }
    r->rtset_lost++;
    return -1;
}
static nr_trans *nr_trans_slot(const nr_reg *r, uint64_t name_key, unsigned state) {   // the entry or the empty slot it would take; NULL = table walked
    const uint64_t h = nr_mix(nr_mix(0xcbf29ce484222325ull, name_key), state);
    for (unsigned p = (unsigned)h & (NR_TRANS_MAX - 1), k = 0; k < NR_TRANS_MAX; p = (p + 1) & (NR_TRANS_MAX - 1), k++) {
        nr_trans *t = &r->trans[p];
        if (!t->seq || (t->name_key == name_key && t->state == state)) return t;
    }
    return NULL;
}
int nr_trans_note(nr_reg *r, uint64_t name_key, const char *name, uint64_t handle, unsigned state) {
    r->trans_calls++;
    nr_trans *t = nr_trans_slot(r, name_key, state);
    if (!t || (!t->seq && r->ntrans >= NR_TRANS_MAX * 3 / 4)) { r->trans_lost++; return -1; }
    if (t->seq) { t->count++; if (t->handle != handle) { t->handle = handle; t->handles++; } return 0; }
    t->name_key = name_key; t->state = state; t->handle = handle; t->count = 1; t->handles = 1; t->seq = ++r->ntrans;
    nr_copy(t->name, sizeof t->name, name);
    return 1;
}
const nr_trans *nr_trans_find(const nr_reg *r, uint64_t name_key, unsigned state) {
    const nr_trans *t = nr_trans_slot(r, name_key, state);
    return t && t->seq ? t : NULL;
}
int nr_pool_note(nr_reg *r, uint64_t key, unsigned efmt, unsigned w, unsigned h) {
    r->pool_calls++;
    for (uint32_t i = 0; i < r->npool; i++) if (r->pool[i].key == key && r->pool[i].efmt == efmt && r->pool[i].w == w && r->pool[i].h == h) { r->pool[i].count++; return 0; }
    if (r->npool >= NR_POOL_MAX) { r->pool_lost++; return -1; }
    nr_pool *p = &r->pool[r->npool];
    p->key = key; p->efmt = efmt; p->w = w; p->h = h; p->count = 1; p->seq = ++r->npool;
    return 1;
}

// ---- checks ----
const char *nr_bad_name(int bit) {
    static const char *const n[NR_BAD_BITS] = {"format", "dims", "mips", "target", "packaged", "no-nvn", "samples", "record", "enum", "program", "engine-format", "filter"};
    return bit >= 0 && bit < NR_BAD_BITS ? n[bit] : "?";
}
int nr_bad_text(unsigned mask, char *out, size_t cap) {
    int k = 0;
    if (cap) out[0] = 0;
    for (int b = 0; b < NR_BAD_BITS; b++) if (mask >> b & 1) { const int n = snprintf(out + k, cap - (size_t)k, "%s%s", k ? "+" : "", nr_bad_name(b)); if (n < 0 || (size_t)(k + n) >= cap) break; k += n; }
    return k;
}
// The switch of CreateTexture (0x91e2a0) / the branches of CreateTextureFromImage (0x91f2e0). NVN targets: 0 1D, 1 2D, 2 3D, 3 1D array,
// 4 2D array, 5 2D multisample, 6 2D multisample array, 8 cube, 9 cube array.
int nr_expect_target(const nr_tex *t) {
    const int cube = (t->eflags & NR_TF_CUBE) != 0;
    if (t->src == NR_SRC_IMAGE_LINEAR || t->src == NR_SRC_IMAGE_PACKAGED) return cube ? 8 : t->d >= 2 ? 2 : (t->d == 1 && t->h == 1) ? 0 : 1;
    if (t->src == NR_SRC_WINDOW) return 1;
    const int one_d = t->w && !t->h && !t->d, two_d = t->w && t->h && !t->d, three_d = t->w && t->h && t->d;
    switch (one_d | two_d << 1 | three_d << 2 | cube << 3 | (t->layers != 1) << 4 | (t->msaa != 1) << 5) {
    case 1: return 0;
    case 4: return 2;
    case 10: return 8;
    case 0x11: return 3;
    case 0x12: return 4;
    case 0x1a: return 9;
    case 0x22: return 5;
    case 0x32: return 6;
    default: return 1;
    }
}
unsigned nr_check_tex(const nr_tex *t) {
    unsigned bad = 0;
    if (t->efmt >= NR_FMT_N) bad |= NR_BAD_EFMT;
    if (!t->nnvn || !t->nvn.obj) return bad | NR_BAD_NO_NVN;
    const nr_nvn_tex *n = &t->nvn;
    const int image = t->src == NR_SRC_IMAGE_LINEAR || t->src == NR_SRC_IMAGE_PACKAGED;
    const int want = nr_expect_nvn_format(t->efmt, t->srgb, t->fmt_override, image);
    if (want >= 0 && want != n->format) bad |= NR_BAD_FORMAT;
    if (t->src == NR_SRC_WINDOW) {   // the window gives no size to compare; only a sane NVN texture
        if (!n->w || !n->h) bad |= NR_BAD_DIMS;
    } else if (image) {
        const uint32_t d = t->layers > t->d ? t->layers : t->d;
        if (n->w != t->w || n->h != t->h || n->d != d) bad |= NR_BAD_DIMS;
        if (n->levels != t->mips) bad |= NR_BAD_MIPS;
    } else {
        const uint32_t d = t->w && t->h && t->d ? t->d : 1;   // the engine makes the depth 1 unless all three sizes are set
        if (n->w != t->w || n->h != t->h || n->d != d) bad |= NR_BAD_DIMS;
        // mips 0 on a colour RT that is not a cube: the engine computes the chain length itself (not checked)
        if (t->mips) { if (n->levels != t->mips) bad |= NR_BAD_MIPS; }
        else if ((t->eflags & NR_TF_CUBE) || !(t->eflags & NR_TF_COLOR_RT)) { if (n->levels != 1) bad |= NR_BAD_MIPS; }
        if (t->msaa == 1 && n->samples > 1) bad |= NR_BAD_SAMPLES;
    }
    const int tgt = nr_expect_target(t);
    if (tgt >= 0 && tgt != n->target) bad |= NR_BAD_TARGET;
    if ((t->src == NR_SRC_IMAGE_PACKAGED) != (n->packaged != 0)) bad |= NR_BAD_PACKAGED;
    return bad;
}
// CreateSampler 0x921e40: filter 0..5 (min filter table 0x1f585e8 = 3,3,5,3,5 for 1..5, else 2; mag linear for 1..5), wrap 0..3 ->
// NVN 1 (repeat), 7 (clamp to edge), 6 (mirrored repeat), 5 (clamp to border), compare 0 = off, 1..7 -> func 2..8, anisotropy as given.
unsigned nr_check_sampler(const nr_sampler *s) {
    static const uint8_t k_min[6] = {2, 3, 3, 5, 3, 5}, k_wrap[4] = {1, 7, 6, 5};
    unsigned bad = 0;
    if (s->filter > 5 || s->wrap[0] > 3 || s->wrap[1] > 3 || s->wrap[2] > 3 || s->compare > 7 || s->aniso > 16) bad |= NR_BAD_ENUM;
    if (!s->nvn.have || !s->nvn.obj) return bad | NR_BAD_NO_NVN;
    if (bad) return bad;
    const nr_nvn_smp *n = &s->nvn;
    if (n->minf != k_min[s->filter] || n->magf != (s->filter ? 1 : 0)) bad |= NR_BAD_FILTER;
    if (n->ws != k_wrap[s->wrap[0]] || n->wt != k_wrap[s->wrap[1]] || n->wr != k_wrap[s->wrap[2]]) bad |= NR_BAD_FILTER;
    if (s->compare ? (n->cmp_mode != 1 || n->cmp_func != s->compare + 1) : n->cmp_mode != 0) bad |= NR_BAD_FILTER;
    if (n->aniso != (float)s->aniso) bad |= NR_BAD_FILTER;
    return bad;
}
// Argument ranges the engine's own code handles (anything else falls to its default branch):
//   blend 0x922530 (srcColor, dstColor, srcAlpha, dstAlpha: factor 0..10; op 0..4; separate alpha 0 / 1)
//   depth-stencil 0x922d10 (depth test, depth write: 0 / 1; depth func 0..7; stencil test 0 / 1; reference byte; stencil func 0..7; two stencil ops 0..7)
//   raster 0x923360 (cull 0..2, fill mode 0..2, two flags, polygon offset: 0 / 1)
unsigned nr_check_state(int kind, const uint8_t *a, int n) {
    static const uint8_t k_max[NR_ST_N][NR_ST_ARGS] = {{10, 10, 10, 10, 4, 1, 0, 0}, {1, 1, 7, 1, 255, 7, 7, 7}, {2, 2, 1, 1, 1, 0, 0, 0}};
    static const uint8_t k_n[NR_ST_N] = {6, 8, 5};
    if (kind < 0 || kind >= NR_ST_N || !a || n != k_n[kind]) return NR_BAD_ENUM;
    for (int i = 0; i < n; i++) if (a[i] > k_max[kind][i]) return NR_BAD_ENUM;
    return 0;
}
unsigned nr_check_shader(const nr_shader *s) {
    unsigned bad = 0; int progs = 0;
    for (int k = 0; k < NR_STAGES; k++) if (s->prog[k]) { progs++; if (!s->key[k][0]) bad |= NR_BAD_PROGRAM; }
    if (!progs) bad |= NR_BAD_NO_NVN;
    if (s->prog[NR_STAGE_CS] && (s->prog[NR_STAGE_VS] || s->prog[NR_STAGE_FS])) bad |= NR_BAD_PROGRAM;   // a compute shader has no other stage
    return bad;
}

// ---- census / summary / TSV ----
void nr_census_take(const nr_reg *r, nr_census *c) {
    memset(c, 0, sizeof *c);
    for (uint32_t i = 0; i < r->st[NR_K_TEX].hi; i++) {
        const nr_tex *t = &r->tex[i];
        if (!t->alive) continue;
        c->alive[NR_K_TEX]++;
        const int src = t->src < NR_SRC_N ? t->src : 0;
        c->tex_src[src]++; c->tex_src_bytes[src] += t->nvn.size; c->tex_cls_bytes[nr_fmt_class(t->efmt)] += t->nvn.size; c->tex_bytes += t->nvn.size;
        if (!t->nnvn) c->tex_no_nvn++;
    }
    for (uint32_t i = 0; i < r->st[NR_K_SHADER].hi; i++) {
        const nr_shader *s = &r->shader[i];
        if (!s->alive) continue;
        c->alive[NR_K_SHADER]++;
        if (s->name[0]) c->shaders_named++;
        for (int k = 0; k < NR_STAGES; k++) if (s->prog[k]) { c->programs++; if (s->key[k][0]) c->programs_keyed++; }
    }
    for (uint32_t i = 0; i < r->st[NR_K_SAMPLER].hi; i++) if (r->sampler[i].alive) c->alive[NR_K_SAMPLER]++;
    for (int k = 0; k < NR_ST_N; k++) for (uint32_t i = 0; i < r->st[NR_K_BLEND + k].hi; i++) if (r->state[k][i].alive) c->alive[NR_K_BLEND + k]++;
    for (uint32_t i = 0; i < r->st[NR_K_VFORMAT].hi; i++) if (r->vformat[i].alive) c->alive[NR_K_VFORMAT]++;
    for (int k = 0; k < NR_BUF_N; k++) for (uint32_t i = 0; i < r->st[NR_K_VB + k].hi; i++) {
        const nr_buffer *b = &r->buf[k][i];
        if (!b->alive) continue;
        c->alive[NR_K_VB + k]++;
        const int dyn = b->usage == NR_USAGE_DYNAMIC;
        c->buf_n[k][dyn]++; c->buf_bytes[k][dyn] += b->size;
    }
}
int nr_summary(const nr_reg *r, char *out, size_t cap) {
    nr_census c; nr_census_take(r, &c);
    uint64_t over = 0, badh = 0;
    for (int k = 0; k < NR_K_N; k++) { over += r->st[k].overflow; badh += r->st[k].bad_handle; }
    const nr_kind_stat *t = &r->st[NR_K_TEX];
    int n = snprintf(out, cap, "textures %u alive (%llu created, %llu destroyed, %llu replaced, %.1f MB), shaders %u (%u named, %u programs, %u keyed), "
                               "samplers %u, vformats %u, vb %u, ib %u, blend %u, depth-stencil %u, raster %u, rt sets %u, transitions %u, pools %u; "
                               "overflow %llu, bad handles %llu",
                     c.alive[NR_K_TEX], (unsigned long long)t->puts, (unsigned long long)t->kills, (unsigned long long)t->replaced, c.tex_bytes / 1048576.0,
                     c.alive[NR_K_SHADER], c.shaders_named, c.programs, c.programs_keyed, c.alive[NR_K_SAMPLER], c.alive[NR_K_VFORMAT], c.alive[NR_K_VB],
                     c.alive[NR_K_IB], c.alive[NR_K_BLEND], c.alive[NR_K_DEPTH], c.alive[NR_K_RASTER], r->nrtset, r->ntrans, r->npool,
                     (unsigned long long)(over + r->rtset_lost + r->trans_lost + r->pool_lost), (unsigned long long)badh);
    if (n < 0) n = 0;
    if (cap && (size_t)n >= cap) n = (int)cap - 1;
    return n;
}
const char *nr_tsv_name(int w) {
    static const char *const n[NR_TSV_N] = {"nr_formats.tsv", "nr_textures.tsv", "nr_shaders.tsv", "nr_samplers.tsv", "nr_states.tsv", "nr_vformats.tsv",
                                            "nr_buffers.tsv", "nr_rtsets.tsv", "nr_transitions.tsv", "nr_pools.tsv"};
    return w >= 0 && w < NR_TSV_N ? n[w] : "nr_unknown.tsv";
}
#define H64(x) ((unsigned long long)(x))
int nr_write_tsv(const nr_reg *r, int which, FILE *o) {
    int rows = 0;
    switch (which) {
    case NR_TSV_FORMATS:
        fprintf(o, "engine_fmt\tname\tnvn\tnvn_srgb\tchannels\tbytes\tcompressed\tclass\n");
        for (unsigned f = 0; f < NR_FMT_N; f++, rows++)
            fprintf(o, "%u\t%s\t%#x\t%#x\t%u\t%u\t%u\t%s\n", f, nr_formats[f].name, nr_formats[f].nvn, nr_formats[f].nvn_srgb, nr_formats[f].channels,
                    nr_formats[f].bytes, nr_formats[f].compressed, nr_cls_name(nr_fmt_class(f)));
        break;
    case NR_TSV_TEXTURES:
        fprintf(o, "handle\talive\tsource\tengine_fmt\tfmt_name\tw\th\td\tmips\tlayers\tmsaa\tflags\tflags2\tsrgb\tfmt_override\tnvn_obj\tnvn_fmt\tnvn_target\tnvn_w\tnvn_h\tnvn_d\t"
                   "nvn_levels\tnvn_samples\tnvn_flags\tnvn_size\tpackaged\tnvn_count\tcreates\tdestroys\treplaced\tbad\n");
        for (uint32_t i = 0; i < r->st[NR_K_TEX].hi; i++) {
            const nr_tex *t = &r->tex[i];
            if (!t->creates) continue;
            fprintf(o, "%#llx\t%u\t%s\t%u\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%#x\t%#x\t%u\t%d\t%#llx\t%#x\t%d\t%u\t%u\t%u\t%u\t%d\t%#x\t%llu\t%u\t%u\t%u\t%u\t%u\t%#x\n",
                    H64(t->handle), t->alive, nr_src_name(t->src), t->efmt, nr_fmt_name(t->efmt), t->w, t->h, t->d, t->mips, t->layers, t->msaa, t->eflags, t->eflags2,
                    t->srgb, t->fmt_override, H64(t->nvn.obj), (unsigned)t->nvn.format, t->nvn.target, t->nvn.w, t->nvn.h, t->nvn.d, t->nvn.levels, t->nvn.samples,
                    (unsigned)t->nvn.flags, H64(t->nvn.size), t->nvn.packaged, t->nnvn, t->creates, t->destroys, t->replaced, t->bad);
            rows++;
        }
        break;
    case NR_TSV_SHADERS:
        fprintf(o, "handle\talive\tname\tpath\tvs_prog\tvs_key\tfs_prog\tfs_key\tcs_prog\tcs_key\tcreates\tgets\treplaced\tbad\n");
        for (uint32_t i = 0; i < r->st[NR_K_SHADER].hi; i++) {
            const nr_shader *s = &r->shader[i];
            if (!s->creates) continue;
            fprintf(o, "%#llx\t%u\t%s\t%s\t%#llx\t%s\t%#llx\t%s\t%#llx\t%s\t%u\t%u\t%u\t%#x\n", H64(s->handle), s->alive, s->name[0] ? s->name : "-", s->path[0] ? s->path : "-",
                    H64(s->prog[0]), s->key[0][0] ? s->key[0] : "-", H64(s->prog[1]), s->key[1][0] ? s->key[1] : "-", H64(s->prog[2]), s->key[2][0] ? s->key[2] : "-",
                    s->creates, s->gets, s->replaced, s->bad);
            rows++;
        }
        break;
    case NR_TSV_SAMPLERS:
        fprintf(o, "handle\talive\tfilter\twrap_u\twrap_v\twrap_w\taniso\tcompare\tnvn_obj\tnvn_min\tnvn_mag\tnvn_ws\tnvn_wt\tnvn_wr\tnvn_cmp_mode\tnvn_cmp_func\tnvn_aniso\tcreates\tbad\n");
        for (uint32_t i = 0; i < r->st[NR_K_SAMPLER].hi; i++) {
            const nr_sampler *s = &r->sampler[i];
            if (!s->creates) continue;
            fprintf(o, "%#llx\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%#llx\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%g\t%u\t%#x\n", H64(s->handle), s->alive, s->filter, s->wrap[0], s->wrap[1], s->wrap[2],
                    s->aniso, s->compare, H64(s->nvn.obj), s->nvn.minf, s->nvn.magf, s->nvn.ws, s->nvn.wt, s->nvn.wr, s->nvn.cmp_mode, s->nvn.cmp_func, (double)s->nvn.aniso,
                    s->creates, s->bad);
            rows++;
        }
        break;
    case NR_TSV_STATES:
        fprintf(o, "kind\thandle\talive\tnargs\ta0\ta1\ta2\ta3\ta4\ta5\ta6\ta7\tcreates\tbad\n");
        for (int k = 0; k < NR_ST_N; k++) for (uint32_t i = 0; i < r->st[NR_K_BLEND + k].hi; i++) {
            const nr_state *s = &r->state[k][i];
            if (!s->creates) continue;
            fprintf(o, "%s\t%#llx\t%u\t%u", nr_state_name(k), H64(s->handle), s->alive, s->nargs);
            for (int a = 0; a < NR_ST_ARGS; a++) fprintf(o, "\t%u", s->arg[a]);
            fprintf(o, "\t%u\t%#x\n", s->creates, s->bad);
            rows++;
        }
        break;
    case NR_TSV_VFORMATS:
        fprintf(o, "handle\talive\tcount\tcreates\telements(stream.usage.b2.b3.type.components)\n");
        for (uint32_t i = 0; i < r->st[NR_K_VFORMAT].hi; i++) {
            const nr_vformat *v = &r->vformat[i];
            if (!v->creates) continue;
            fprintf(o, "%#llx\t%u\t%u\t%u\t", H64(v->handle), v->alive, v->count, v->creates);
            for (unsigned e = 0; e < v->kept; e++) fprintf(o, "%s%u.%u.%u.%u.%u.%u", e ? " " : "", v->elem[e][0], v->elem[e][1], v->elem[e][2], v->elem[e][3], v->elem[e][4], v->elem[e][5]);
            fprintf(o, "%s\n", v->kept ? "" : "-");
            rows++;
        }
        break;
    case NR_TSV_BUFFERS:
        fprintf(o, "kind\thandle\talive\tsize\tcount\tindex_size\tusage\tclass\tnvn_obj\tnvn_size\tcreates\tdestroys\treplaced\n");
        for (int k = 0; k < NR_BUF_N; k++) for (uint32_t i = 0; i < r->st[NR_K_VB + k].hi; i++) {
            const nr_buffer *b = &r->buf[k][i];
            if (!b->creates) continue;
            fprintf(o, "%s\t%#llx\t%u\t%llu\t%u\t%u\t%u\t%s\t%#llx\t%llu\t%u\t%u\t%u\n", k ? "index" : "vertex", H64(b->handle), b->alive, H64(b->size), b->count, b->index_size,
                    b->usage, b->usage == NR_USAGE_DYNAMIC ? "dynamic" : "static", H64(b->nvn_obj), H64(b->nvn_size), b->creates, b->destroys, b->replaced);
            rows++;
        }
        break;
    case NR_TSV_RTSETS:
        fprintf(o, "seq\tcount\tn\tdepth\tc0\tc1\tc2\tc3\tc4\tc5\tc6\tc7\n");
        for (uint32_t i = 0; i < NR_RTSET_MAX; i++) {
            const nr_rtset *s = &r->rtset[i];
            if (!s->seq) continue;
            fprintf(o, "%u\t%llu\t%u\t%#llx", s->seq, H64(s->count), s->n, H64(s->depth));
            for (unsigned c = 0; c < NR_RT_COLORS; c++) fprintf(o, "\t%#llx", H64(c < s->n ? s->color[c] : 0));
            fputc('\n', o);
            rows++;
        }
        break;
    case NR_TSV_TRANSITIONS:
        fprintf(o, "seq\tname\tstate\thandle\tcount\thandle_changes\tname_key\n");
        for (uint32_t i = 0; i < NR_TRANS_MAX; i++) {
            const nr_trans *t = &r->trans[i];
            if (!t->seq) continue;
            fprintf(o, "%u\t%s\t%u\t%#llx\t%llu\t%u\t%#llx\n", t->seq, t->name[0] ? t->name : "-", t->state, H64(t->handle), H64(t->count), t->handles, H64(t->name_key));
            rows++;
        }
        break;
    case NR_TSV_POOLS:
        fprintf(o, "seq\tkey\tengine_fmt\tfmt_name\tw\th\tcount\n");
        for (uint32_t i = 0; i < r->npool; i++, rows++)
            fprintf(o, "%u\t%#llx\t%u\t%s\t%u\t%u\t%llu\n", r->pool[i].seq, H64(r->pool[i].key), r->pool[i].efmt, nr_fmt_name(r->pool[i].efmt), r->pool[i].w, r->pool[i].h, H64(r->pool[i].count));
        break;
    default: return -1;
    }
    return rows;
}
