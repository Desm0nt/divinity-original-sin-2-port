// [DOS2] NR-08: descriptors of the native renderer (DM_NR_DESC, default 0; docs/dos2/nr/NR-08.md). Pure C: no Vulkan, no engine, no host
// types. The engine side (dm/gfx/vulkan/engine/xbg_nr_desc.inc) feeds it; dm/gfx/vulkan/engine/tests/nr_desc_test.c tests it on the PC.
//
// A draw's descriptor set is split BY CHANGE FREQUENCY into two parts (logical sets; why they stay one VkDescriptorSet on every device
// class is in NR-08.md §2):
//   texture part   image views + samplers of the slots the shader set reads, the support block and the bundle's c1 constants: fixed by
//                  (shader set, texture / sampler handles in those slots, pools, viewport size). One entry per distinct key, found by
//                  the backend's texture generation (nothing rebound since the last draw of this shader set = the same entry, no
//                  hashing) or by a hash of the handles; its set is written ONCE.
//   uniform part   every uniform block of the game lives in the engine's constant ring (NR-06: one VkBuffer): a block is its offset in
//                  that buffer. Where the device allows it the offsets are DYNAMIC OFFSETS (the set is never rewritten); a block that
//                  cannot be dynamic (more blocks than maxDescriptorSetUniformBuffersDynamic - Mali / PowerVR allow 8 - or dynamic
//                  uniform buffers off: PowerVR) is a STATIC block: its offsets are part of a set variant (nr_dfifo: a set is rewritten
//                  only after every submission that could read it retired).
// Read-after-write hazards (prepare_read) are checked only for images that were ever written by the GPU; a material texture (uploaded,
// never an attachment / storage / copy target) already checked in this submission needs nothing (nr_dimg_skip).
//
// Tables are bounded; a full table returns -1 and the caller flushes or falls back to the old path. Not thread safe (the translation
// thread only).
#ifndef DM_NR_DESC_H
#define DM_NR_DESC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRD_MAX_ROWS = 48, NRD_MAX_UBO = 16, NRD_MAX_IMG = 32, NRD_MAX_DYN = 32, NRD_STAGES = 5,
       NRD_KEY_FIXED = 11,                              // 5 shaders, viewport word, 5 pool words
       NRD_KEY_WORDS = NRD_KEY_FIXED + 2 * NRD_MAX_IMG };
// descriptor types / sources as the bundle's binding table names them (VkDescriptorType values, XBG_SOURCE_* of xbg_game.h)
enum { NRD_T_COMBINED = 1, NRD_T_SAMPLED = 2, NRD_T_UBO = 6 };
enum { NRD_S_SUPPORT = 1, NRD_S_UNIFORM = 2, NRD_S_C1 = 4, NRD_S_DRIVER0 = 5, NRD_S_DRIVER2 = 6, NRD_S_TEXTURE = 7 };
// one row of a bundle's binding table (= xbg_shader_binding, 8 u32)
typedef struct nr_dbind { uint32_t binding, type, count, bytes, source, slot, sampler_slot, flags; } nr_dbind;

// ---- the plan of a shader set: one row per descriptor, in the order the old path writes them (stage 0..4, table order) ----
enum { NRD_K_RING_DYN,     // uniform block of the game, dynamic offset into the ring buffer
       NRD_K_RING_STAT,    // uniform block of the game, offset baked into the set (a set variant per offset tuple)
       NRD_K_FIXED_DYN,    // support block / c1 constants as a dynamic buffer (the offset never changes)
       NRD_K_FIXED_STAT,   // support block / c1 constants at a baked offset
       NRD_K_IMAGE };      // texture (+ sampler)
// why a shader set cannot take the NR path (0 = it can)
enum { NRD_OK = 0, NRD_NE_TYPE, NRD_NE_SOURCE, NRD_NE_DRIVER, NRD_NE_ARRAY, NRD_NE_SLOT, NRD_NE_ROWS, NRD_NE_NOSHADER, NRD_NE_LIMIT, NRD_NE_N };
const char *nr_dne_name(int why);
typedef struct nr_drow {
    uint8_t kind, ns, slot, samp;        // ns: NVN stage of the table; slot: uniform / texture slot; samp: sampler slot, 0xff = the texture's own
    uint8_t dyn_at, stat_at, source, st; // index in the dynamic offsets (0xff none), in the static tuple (0xff none); XBG source; shader stage
    uint32_t binding, bytes;             // Vulkan binding; block bytes of the table (0 = unknown: the bound size, at most 65536)
} nr_drow;
typedef struct nr_dplan {
    int why;                             // NRD_OK or NRD_NE_*
    uint8_t nrow, nubo, ndyn, nstat, nimg, tex_stages, ubo_stages, pad;   // nubo: ring blocks; ndyn: dynamic offsets; nstat: static ring blocks
    uint8_t ubo_row[NRD_MAX_UBO], img_row[NRD_MAX_IMG];
    nr_drow row[NRD_MAX_ROWS];
} nr_dplan;
// One stage's table: rows b[0..n), NVN stage ns, dyn[i] != 0 when the engine made row i a dynamic uniform buffer (UBO_DYN).
typedef struct nr_dstage { const nr_dbind *b; const uint8_t *dyn; uint32_t n, ns, present; } nr_dstage;
// Builds the plan of the shader set st[0..NRD_STAGES). Returns p->why.
int nr_dplan_build(nr_dplan *p, const nr_dstage st[NRD_STAGES]);
// The device limits of an eligible plan: textures of one stage against min(maxPerStageDescriptorSamplers, ...SampledImages), uniform
// buffers of one stage against maxPerStageDescriptorUniformBuffers, dynamic ones of the set against
// maxDescriptorSetUniformBuffersDynamic. Over a limit: p->why = NRD_NE_LIMIT (the shader set stays on the old path, which is where
// such a layout was made before). Returns p->why.
int nr_dplan_limits(nr_dplan *p, uint32_t stage_images, uint32_t stage_ubos, uint32_t set_dynamic);

// ---- more ring blocks than the device has dynamic uniform buffers: which stay dynamic ----
// changes[i] = how often block i's address differed from the draw before (any counter with that order). The `limit` blocks that
// change most stay dynamic (ties: the lower index), the rest become static (set variants). dyn[i] = 1 / 0. Returns the static count.
unsigned nr_dsplit(const uint32_t *changes, unsigned n, unsigned limit, uint8_t *dyn);

// ---- key -> entry ----
uint64_t nr_dkey_hash64(const uint64_t *key, unsigned n);
typedef struct nr_dtab {
    uint32_t max, cap, n;                // entries (dense ids 0..n-1), slots of the open-addressing index (power of two)
    uint32_t *idx;                       // id + 1, 0 = empty
    uint64_t *hash, *key;                // per entry: hash, NRD_KEY_WORDS words
    uint8_t *nkey;
} nr_dtab;
int nr_dtab_init(nr_dtab *t, uint32_t max_entries);    // 0 ok, -1 no memory / bad size
void nr_dtab_free(nr_dtab *t);
void nr_dtab_clear(nr_dtab *t);                        // every id is gone
int32_t nr_dtab_find(const nr_dtab *t, uint64_t h, const uint64_t *key, unsigned n);   // id or -1
int32_t nr_dtab_add(nr_dtab *t, uint64_t h, const uint64_t *key, unsigned n);          // new id (the key must be absent), -1 = full

// ---- set recycler: a set may be rewritten only when no recorded or flying submission can read it ----
// The backend records submission `serial` into one of two frame slots; before it records, the slot that ran serial - 2 has retired
// (xbg_game.cpp recording_slot; the rule NR-06's ring mirror uses). So a set last bound at `stamp` is free at serial >= stamp + 2.
typedef struct nr_dnode { uint64_t set; uint32_t stamp, gen; int32_t prev, next; } nr_dnode;
typedef struct nr_dfifo { nr_dnode *node; uint32_t cap, n; int32_t head, tail; uint64_t taken, fresh; } nr_dfifo;   // a list in stamp order
void nr_dfifo_init(nr_dfifo *f);
void nr_dfifo_free(nr_dfifo *f);
// The oldest node, if it was last bound at least `age` submissions ago (age >= 2: its set may be rewritten; its gen moves: holders of
// the old contents see it); re-stamped `serial` and queued last. -1 = none is that old (make a new set and nr_dfifo_push it).
// age 2 = as soon as it is safe (sets rewritten every draw); a larger age keeps the sets of idle entries (a set written once is cheap
// to keep and costs a descriptor update to lose).
enum { NRD_AGE_SAFE = 2, NRD_AGE_IDLE = 600 };
int32_t nr_dfifo_take(nr_dfifo *f, uint32_t serial, uint32_t age);
int32_t nr_dfifo_push(nr_dfifo *f, uint64_t set, uint32_t serial);   // a new set, bound at `serial`; its node, -1 = no memory
// The holder binds the same contents again: 1 = still its set (re-stamped, queued last), 0 = the node was recycled since (gen differs).
int nr_dfifo_use(nr_dfifo *f, int32_t node, uint32_t gen, uint32_t serial);

// ---- the read hazard of one image of an entry ----
enum { NRD_I_SKIP = 0,   // nothing to do: get_image and prepare_read would be no-ops
       NRD_I_FIRST,      // first use in this submission (or no contents yet): the full path (upload / content check)
       NRD_I_RT,         // written by the GPU at some time (render target, storage, copy target): the hazard check
       NRD_I_LAYOUT,     // not in the layout sets read
       NRD_I_HOOK,       // a read hook that wants every read is armed
       NRD_I_N };
typedef struct nr_dimg_in {
    uint64_t last_write, storage_write, op_write;   // the image's write marks (0 = never)
    uint8_t checked, uploaded, gpu_written, general, hook;   // checked: content checked in this submission
} nr_dimg_in;
static inline int nr_dimg_skip(const nr_dimg_in *x) {
    if (x->hook) return NRD_I_HOOK;
    if (x->gpu_written || x->last_write || x->storage_write || x->op_write) return NRD_I_RT;
    if (!x->checked || !x->uploaded) return NRD_I_FIRST;
    if (!x->general) return NRD_I_LAYOUT;
    return NRD_I_SKIP;
}

#ifdef __cplusplus
}
#endif
#endif
