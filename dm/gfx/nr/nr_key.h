// [DOS2] NR-02: draw identity of the native renderer (DM_NR_KEY, default 0; docs/dos2/nr/NR-02.md). Pure C on top of nr_reg.h: no
// Vulkan, no game addresses, no host types. The front end (dm/host/hle_dos2_nr_key.inc) reads the renderer's state cache at every engine
// draw and feeds the tables here; dm/gfx/vulkan/engine/tests/nr_key_test.cpp tests them on the PC. Later steps reuse them:
//   NR-04 (shaders by name)      nr_shader_find_name / nr_shader_find_key, the names TSV (name <-> pack key of every stage)
//   NR-07 (pipelines by handles) nr_dkey + nr_key_note: one dense id per distinct (shader, blend, depth-stencil, raster, vertex format,
//                                primitive, alpha test, RT format set) - a direct table pipeline[id] - and why a key is not eligible
//   NR-08 (descriptors)          nr_texset_hash / nr_texset_note: one dense id per distinct texture set (texture + sampler pool ids of the
//                                vertex and fragment stages), with how many of its slots are render targets (the only ones that need a
//                                read-after-write hazard check) and how many ids the registry does not know; nr_texid_*: pool id -> handle
//   bundle header                nr_layout_word: the NR descriptor layout of a translated bundle, kept in xbg_shader_header.reserved
// Not thread safe: the caller serialises every call on one nr_keys (the front end: one mutex; lock order key mutex -> registry mutex).
// Every table is bounded; a full table returns id 0 / NULL and counts the loss, nothing fails harder.
#ifndef DM_NR_KEY_H
#define DM_NR_KEY_H
#include "nr_reg.h"
#ifdef __cplusplus
extern "C" {
#endif

// ---- the engine call that flushed the texture slots right before an NVN draw (one per call site of the flush; the front end maps
// return addresses to these kinds) ----
enum { NR_DS_UNKNOWN, NR_DS_LIST_IDX, NR_DS_IDX_INST, NR_DS_LIST_ARRAYS, NR_DS_FULL, NR_DS_DIRECT_IDX, NR_DS_DIRECT_ARRAYS, NR_DS_FLUSH,
       NR_DS_COMPUTE, NR_DS_N };
const char *nr_dsite_name(int kind);
int nr_dsite_is_draw(int kind);    // 1: an NVN draw follows on the same thread; 0: a flush only (no draw) / a dispatch

// ---- what the renderer's state cache holds at a draw ----
enum { NR_TS_STAGES = 2, NR_TS_SLOTS = 32 };   // vertex + fragment stage, 32 texture slots each (clean marker of the dirty range = 32)
typedef struct nr_dkey {
    uint64_t shader, blend, depth, raster, vformat;   // engine handles (0 = none bound)
    uint32_t blend_arg;                               // the second argument of the blend bind (colour write mask word, -1 = default)
    uint16_t rtfmt;                                   // nr_rtfmt_note id of the render-target formats (0 = table full)
    uint8_t prim, alpha_test, pad[4];                 // engine primitive byte; alpha test on (alpha reference >= 0)
} nr_dkey;
// why a key cannot be drawn by a native path (a mask; 0 = eligible). NR_NE_FULL is a property of the draw, not of the key.
enum { NR_NE_SHADER = 1, NR_NE_COMPUTE = 2, NR_NE_PACKKEY = 4, NR_NE_STATE = 8, NR_NE_VFORMAT = 16, NR_NE_RT = 32, NR_NE_FULL = 64, NR_NE_LOST = 128,
       NR_NE_BITS = 8 };
const char *nr_ne_name(int bit);
int nr_ne_text(unsigned mask, char *out, size_t cap);

typedef struct nr_kent { nr_dkey k; uint64_t h, draws; uint32_t id, why, first_win, last_win; } nr_kent;
typedef struct nr_texset { uint64_t h, draws; uint32_t id, first_win, last_win; uint8_t nslots, nrt, nmat, nunknown; } nr_texset;
typedef struct nr_rtfmt { uint64_t uses; uint32_t id; uint8_t n, fmt[NR_RT_COLORS], depth, msaa, unknown; } nr_rtfmt;   // fmt 0xff = slot empty
typedef struct nr_texid { uint32_t id, used; uint64_t handle; } nr_texid;
enum { NR_KEY_MAX = 16384, NR_TEXSET_MAX = 65536, NR_RTFMT_MAX = 256, NR_TEXID_MAX = 65536, NR_FMT_NONE = 0xff, NR_FMT_UNKNOWN = 0xfe };
typedef struct nr_keys {
    nr_kent *key; nr_texset *ts; nr_rtfmt *rtf; nr_texid *tid;   // key / ts / rtf dense by id - 1; tid open addressing by pool id
    uint32_t *kidx, *tsidx;                                       // open addressing hash -> id (2 x the dense size)
    uint32_t nkey, nts, nrtf, ntid, win, win_keys, win_ts;   // win: window number (nr_keys_window); win_*: distinct ids seen in it
    uint64_t key_lost, ts_lost, rtf_lost, tid_lost;
} nr_keys;
nr_keys *nr_keys_new(void);             // all tables in one place (install time). NULL = no memory. nr_keys_free(NULL) is fine.
void nr_keys_free(nr_keys *k);
void nr_keys_window(nr_keys *k);        // a new counting window: win_keys / win_ts restart

// The render-target format set of a draw: engine formats of the colour slots (registry by handle; 0 = empty slot) and of the depth target,
// the sample count of the first one found. Returns its id (1..), 0 = table full. `unknown` is set when a handle is not in the registry.
uint16_t nr_rtfmt_note(nr_keys *k, const nr_reg *r, const uint64_t color[NR_RT_COLORS], uint64_t depth);
const nr_rtfmt *nr_rtfmt_get(const nr_keys *k, uint16_t id);
// The key of a draw: its entry (id 1..), created on first sight with `why` computed from the registry; NULL = table full (counted).
// *fresh = 1 when it was created now. Counts the draw on the entry and the window. r may be NULL: a known key is found without the
// registry; a new one then returns NULL with *fresh = -1 (nothing counted) and the caller calls again with the registry (locked).
const nr_kent *nr_key_note(nr_keys *k, const nr_reg *r, const nr_dkey *key, int *fresh);
uint64_t nr_dkey_hash(const nr_dkey *key);
unsigned nr_dkey_why(const nr_reg *r, const nr_keys *k, const nr_dkey *key);   // the NR_NE_* mask of a key (without NR_NE_FULL / LOST)
const nr_kent *nr_key_get(const nr_keys *k, uint32_t id);

// Texture sets. hash: over the texture and sampler pool ids of both stages (-1 = empty slot). note: the entry (id 1..) of that hash,
// created with its slots classified through the pool id map and the registry; NULL = table full. *fresh and r = NULL as above.
// Two sets with the same 64-bit hash count as one (not detected).
uint64_t nr_texset_hash(const int32_t tex[NR_TS_STAGES][NR_TS_SLOTS], const int32_t smp[NR_TS_STAGES][NR_TS_SLOTS]);
const nr_texset *nr_texset_note(nr_keys *k, const nr_reg *r, uint64_t h, const int32_t tex[NR_TS_STAGES][NR_TS_SLOTS], int *fresh);
const nr_texset *nr_texset_get(const nr_keys *k, uint32_t id);
void nr_texset_count(nr_keys *k, uint32_t id);   // one more draw on a set that did not change (no hashing): draws + window
// pool id (the engine stores it at NVN texture object + 0xc0 after nvnTexturePoolRegisterTexture) -> engine texture handle
int nr_texid_put(nr_keys *k, uint32_t id, uint64_t handle);    // 1 = stored (a later put of the same id replaces it), 0 = map full
uint64_t nr_texid_get(const nr_keys *k, uint32_t id);          // 0 = unknown
const nr_tex *nr_texid_tex(const nr_keys *k, const nr_reg *r, uint32_t id);   // the registry entry, if that handle is still alive

// ---- small registry lookups the key needs (not in nr_reg.c: kept to this module) ----
const nr_state *nr_state_find(const nr_reg *r, int kind, uint64_t handle);   // alive entry of this very handle
const nr_vformat *nr_vformat_find(const nr_reg *r, uint64_t handle);
// ---- name <-> pack key (NR-04): linear scans over the registry, for setup / logs, not per draw ----
const nr_shader *nr_shader_find_name(const nr_reg *r, const char *name);           // alive shader of that ShaderManager name
const nr_shader *nr_shader_find_key(const nr_reg *r, const char *key, int *stage); // alive shader with a stage of that pack key

// ---- the NR descriptor layout of a translated bundle (xbg_shader_header.reserved; the old reader ignores that word) ----
// Input: the bundle's binding table, 8 u32 per binding (binding, type, count, bytes, source, slot, sampler_slot, flags), `n` rows.
// NR layout: set 0 = the uniform blocks (source 2) in table order, each a dynamic-offset UBO into the constant ring; set 1 = the
// textures (source 7) in table order. Word: bits 28..31 = 0xA (present), 24..27 version 1, 20..23 uniform blocks (15 = 15 or more),
// 14..19 textures (63 = 63 or more), 8..13 reserved 0, 0..7 NR_LW_* flags.
enum { NR_LW_C1 = 1, NR_LW_SUPPORT = 2, NR_LW_DRIVER = 4, NR_LW_STORAGE = 8, NR_LW_ARRAYS = 16, NR_LW_OTHER = 32, NR_LW_FITS = 64, NR_LW_CLAMPED = 128 };
enum { NR_LW_TAG = 0xA, NR_LW_VERSION = 1, NR_LW_MAX_UBO = 8 };   // NR_LW_FITS: no storage / image / array / unknown binding and <= 8 blocks
uint32_t nr_layout_word(const uint32_t *bindings8, unsigned n);
int nr_layout_valid(uint32_t word);                 // tag and version present
unsigned nr_layout_ubos(uint32_t word), nr_layout_textures(uint32_t word), nr_layout_flags(uint32_t word);
int nr_layout_text(uint32_t word, char *out, size_t cap);

// ---- TSV (with DM_NR_DUMP) ----
enum { NR_KTSV_KEYS, NR_KTSV_TEXSETS, NR_KTSV_RTFMTS, NR_KTSV_NAMES, NR_KTSV_N };
const char *nr_ktsv_name(int which);                // "nr_keys.tsv", "nr_texsets.tsv", "nr_rtfmts.tsv", "nr_names.tsv"
int nr_keys_write_tsv(const nr_keys *k, const nr_reg *r, int which, FILE *o);   // header + rows; returns the rows

#ifdef __cplusplus
}
#endif
#endif
