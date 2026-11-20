// [DOS2] NR-05 (DM_NR_TEX, default 0): material textures by engine events. Pure C core: no Vulkan, no pthread, no getenv, no game
// addresses - dm/host/hle_dos2_nr.c wraps it with one mutex, a worker thread and the hooks of NR-01; the Vulkan engine
// (xbg_game.cpp namespace nrtex) asks it at image creation / first upload; dm/gfx/vulkan/engine/tests/nr_tex_test.c tests it on the PC.
//
// A material texture (CreateTextureFromImage: packaged or linear image data, docs/dos2/NATIVE_RENDERER_PLAN.md §0) is complete when the
// engine call returns. At that event the front end records it here by its NVN texture serial and queues a job; the worker deswizzles
// the storage into a host blob laid out exactly as get_image_slow's DM_GFX_DESWZ_DIRECT staging (nr_tex_layout: per layer, per level,
// 16-byte aligned starts, pitch = blocks x bytes per block) and, when the ASTC texture cache is active, takes its key (CRC-32 +
// Adler-32 of the storage). The first upload then copies the blob instead of deswizzling on the translation thread.
// Guard: a probe (nr_tex_probe: 33 samples of 32 bytes spread over the storage) is taken around the read; a blob / key is handed
// out only while the storage still gives the same probe, so a late write by the streamer falls back to the old path.
// Not thread safe by itself: the caller serialises every call except nr_tex_job_run (the expensive part, no shared state).
#ifndef DM_NR_TEX_H
#define DM_NR_TEX_H
#include <stddef.h>
#include <stdint.h>
#include "../nvn/nvn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { NRT_OFF = 0, NRT_ON = 1, NRT_VERIFY = 2, NRT_KEEP = 3 };   // DM_NR_TEX values (see hle_dos2_nr.c)

// NR-05 fix (docs/dos2/nr/NR-05-fix.md): the engine's start decision (xbg_game.cpp nrtex::init / poll). The engine's device init runs
// in parallel with the host's preload and came ~190 ms before the front end set DM_NR_TEX on the phone: reading the mode once there gave
// 0 for good. host_mode / host_set: the front end's g_dm_nr_tex / g_dm_nr_tex_set (1 once its install returned, release); NULL = no
// front end (mode 0) / a front end that publishes no flag (its value as it is now). 1 = decided, *mode = the mode to run (0..3);
// 0 = the install still runs: ask again later (before any guest code it has returned, so the first engine texture sees the decision).
static inline int nrt_engine_decide(const int *host_mode, const int *host_set, int *mode) {
    *mode = 0;
    if (!host_mode) return 1;
    if (host_set && !__atomic_load_n(host_set, __ATOMIC_ACQUIRE)) return 0;
    const int m = __atomic_load_n(host_mode, __ATOMIC_ACQUIRE);
    *mode = m >= NRT_ON && m <= NRT_KEEP ? m : NRT_OFF;
    return 1;
}

// ---- pure helpers ----
uint64_t nr_tex_probe(const void *mem, uint64_t size);   // samples (whole range below 1 KB); 0 never returned for size > 0
uint64_t nr_tex_hash(const void *mem, uint64_t size);    // full content hash (mode 2 checks only)
// The staging layout of get_image_slow for a colour block-linear texture taken as it is (not BC-decoded, not linear): one sub per
// layer and level, layer outer. Returns the sub count, -1 when the texture is not one the worker can prepare (multisample, linear,
// unknown format, more than NRT_SUBS subresources, empty).
enum { NRT_SUBS = 256 };
typedef struct nrt_sub { uint64_t off, bytes; uint32_t pitch, level, layer, w, h, depth; } nrt_sub;
int nr_tex_layout(const nvn_texdesc *d, const nvn_texlayout *L, nrt_sub *subs, int cap, uint64_t *total);
// [dos2-nr22fix] DM_NR_TEXSTREAM=2 (and NR-05 =2): two stagings of this layout compared as vkCmdCopyBufferToImage reads them. Every
// subresource starts 16-byte aligned, so a level whose bytes are not a multiple of 16 (BC1 / BC4 8-byte blocks, a 1x1 / 2x1 tail mip,
// odd plain sizes) leaves a gap before the next one; no copy reads it and neither path writes it (the old path's ring buffer and the
// worker's slice keep whatever was there). a: `a_cap` bytes (the worker's slice, rounded up to 16), b: `bn` bytes (the old path's).
// NRT_CMP_SAME every byte of [0, total) equal; NRT_CMP_PAD every subresource equal, only gap bytes differ (not a mismatch);
// NRT_CMP_DIFF a subresource differs (*at its index, *off the first differing byte from the staging start); NRT_CMP_LAYOUT not this
// layout (bn != total, a_cap < total, no layout).
enum { NRT_CMP_SAME = 0, NRT_CMP_PAD = 1, NRT_CMP_DIFF = 2, NRT_CMP_LAYOUT = 3 };
int nr_tex_cmp_staged(const nvn_texdesc *d, const nvn_texlayout *L, const uint8_t *a, uint64_t a_cap, const uint8_t *b, uint64_t bn, int *at, uint64_t *off);
// The copy regions the old path recorded (VkBufferImageCopy, offsets from its staging start) against the regions this layout gives
// (nrtex::copies_of, used by DM_NR_TEXSTREAM=1 / DM_NR_TEX=1): 1 = the same list, field for field; 0 = not (*at the first that differs).
typedef struct nrt_region { uint64_t off; uint32_t row_len, img_h, level, layer, layers, x, y, z, w, h, depth; } nrt_region;
int nr_tex_regions_match(const nvn_texdesc *d, const nvn_texlayout *L, const nrt_region *r, int n, int *at);
// The textures the ASTC texture cache keys (xbg_game.cpp astccache::cacheable): 2D, one layer, BC1..BC3, block-linear.
int nr_tex_astc_cacheable(const nvn_texdesc *d, const nvn_texlayout *L, const void *mem);

// ---- the table ----
typedef struct nrt_src {   // what the engine call made (copied at the event)
    nvn_texdesc d; nvn_texlayout L;
    const uint8_t *mem;
    uint64_t serial, handle, obj;   // obj: the NVN texture object (the engine's deferred destroy names only that)
    uint8_t src, efmt;     // nr_reg.h NR_SRC_*, engine format
} nrt_src;
enum { NRT_ST_NONE, NRT_ST_QUEUED, NRT_ST_WORKING, NRT_ST_READY };   // blob state of a live entry
// why no blob was handed out (nr_tex_take) / why no job was queued (nr_tex_create)
enum { NRT_WHY_OK, NRT_WHY_UNKNOWN, NRT_WHY_PENDING, NRT_WHY_NONE, NRT_WHY_STALE, NRT_WHY_EXPIRED, NRT_WHY_SMALL, NRT_WHY_BIG, NRT_WHY_FMT,
       NRT_WHY_QUEUE, NRT_WHY_LAYOUT, NRT_WHY_FAILED, NRT_WHY_CHANGING, NRT_WHY_NOMEM, NRT_WHY_N };
const char *nr_tex_why_name(int why);
typedef struct nrt_entry {
    uint64_t serial;          // 0 = empty slot
    uint64_t handle, size, t_create, probe, hash0, blob_bytes, obj;   // blob_bytes: the budget this entry holds (heap blob, or a ts object's staging)
    const uint8_t *mem;
    uint8_t *blob;            // heap blob, or (ts set) the mapped staging inside the engine's object (never freed here); NULL for an ASTC ts
    void *ts;                 // NR-22 DM_NR_TEXSTREAM: the engine's prepared object (VkImage + memory + staging), released through the sink
    uint8_t ts_kind;          // NRT_TS_DESW / NRT_TS_ASTC
    uint32_t crc, adler, writes;   // writes: nvnTextureWriteTexels into it since the create event
    uint8_t state, alive, src, efmt, have_key, why, taken;
    uint8_t engine_dead;      // the engine queued it for its deferred destroy (handle gone, NVN object not finalized yet)
    uint16_t fmt;             // NVN format
} nrt_entry;
typedef struct nrt_info { uint64_t handle, hash0, t_create; uint32_t writes; uint8_t alive, src, efmt, state, have_key, why, engine_dead; } nrt_info;
// ---- NR-22 (DM_NR_TEXSTREAM): the engine's maker of prepared images (xbg_game.cpp namespace nrts) ----
// With a sink the worker asks the engine for a staging target instead of a heap blob: the engine creates the VkImage + its memory and
// hands out mapped host-visible staging, the worker deswizzles straight into it (NRT_TS_DESW). For a texture the ASTC texture cache
// holds (key known), make_astc creates the ASTC image and reads the entry's blocks into staging (NRT_TS_ASTC). The translator later
// takes the object (nr_tex_take_ts) and only records the copy. release: an object the translator never took (stale, gone, expired,
// written, engine-dead, a job dropped) - called with the caller's lock held, so it must only queue the object (the engine destroys
// it later off that lock). NULL from make / make_astc = refused: the job goes on as before (heap blob / key only).
enum { NRT_TS_NONE = 0, NRT_TS_DESW = 1, NRT_TS_ASTC = 2 };
typedef struct nrt_sink {
    void *(*make)(void *ctx, const nrt_src *s, uint64_t bytes, uint8_t **dst);
    void *(*make_astc)(void *ctx, const nrt_src *s, uint32_t crc, uint32_t adler, uint64_t *bytes);
    void (*release)(void *ctx, void *obj);
    void *ctx;
} nrt_sink;
typedef struct nrt_job {     // nr_tex_job_begin fills s / bytes / want_*; nr_tex_job_run the rest
    nrt_src s; nrt_sub sub[NRT_SUBS]; int nsub;
    uint64_t bytes, probe, ns, ts_bytes;
    uint8_t *blob;
    uint32_t crc, adler;
    uint8_t want_blob, want_key, ok, have_key, why;
    nrt_sink sink; void *ts; uint8_t ts_kind, fmt_ok, ts_refused;   // NR-22: a copy of the core's sink; fmt_ok: the translator took this format as it is
} nrt_job;
typedef int (*nrt_key_fn)(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler);   // 0 = done (CRC-32 + Adler-32, zlib)
typedef struct nrt_stats {
    uint64_t events, events_src[8], queued, prepared, prepared_bytes, prepared_ns, keys, taken, taken_bytes, key_served, key_stale,
             expired, expired_bytes, gone, gone_ready, wrapped, written, engine_gone, omap_lost, why[NRT_WHY_N];
    uint64_t live_bytes, peak_bytes;   // blobs held (READY + taken but not dropped yet + reserved by a running job)
    uint64_t ts_made, ts_astc, ts_refused, ts_released, ts_taken, ts_taken_bytes;   // NR-22: prepared objects made (deswizzled / ASTC), refused by the engine, released untaken, taken
} nrt_stats;
enum { NRT_CAP = 1 << 15, NRT_QUEUE = 4096, NRT_OMAP = 1 << 16 };
typedef struct nrt_omap { uint64_t obj, serial; } nrt_omap;   // NVN object -> serial, direct-mapped by a hash of the object
typedef struct nr_tex_core {
    nrt_entry *e;               // NRT_CAP, direct-mapped by serial (a newer serial in the slot of a live older one evicts it: counted)
    nrt_src *q; uint32_t qhead, qtail;   // job ring (serials re-checked when the job starts)
    nrt_omap *omap;             // NRT_OMAP
    uint64_t budget, min_bytes, max_bytes;
    uint8_t deny[32], allow[32];  // NVN format verdicts of the translator (bit set): a denied format gets no blob
    uint8_t want_key;           // the ASTC texture cache is active
    nrt_sink sink;              // NR-22: all NULL = no prepared objects (NR-05 alone)
    nrt_stats st;
} nr_tex_core;

nr_tex_core *nr_tex_new(uint64_t budget, uint64_t min_bytes, uint64_t max_bytes);   // NULL = no memory
void nr_tex_free(nr_tex_core *c);
// The create event. want_job: queue a worker job (blob and / or key). hash0: full hash taken by the caller (mode 2), else 0.
// Returns NRT_WHY_OK when a job was queued, else the reason (the entry is recorded either way).
int nr_tex_create(nr_tex_core *c, const nrt_src *s, uint64_t hash0, uint64_t now, int want_job);
void nr_tex_gone(nr_tex_core *c, uint64_t serial);    // nvnTextureFinalize of this serial: the entry dies, a READY blob is freed
int nr_tex_written(nr_tex_core *c, uint64_t serial);  // nvnTextureWriteTexels into it: writes + 1, blob / key / job dropped; 1 = a live entry
// The engine's deferred texture destroy saw this NVN object (its handle is gone; the object is finalized frames later): the entry is
// engine-dead (blob dropped). No new frame draws it, so a later change of its storage is another owner's data. 1 = newly marked.
int nr_tex_engine_gone(nr_tex_core *c, uint64_t obj);
int nr_tex_find(const nr_tex_core *c, uint64_t serial, nrt_info *out);   // 1 = an entry of this very serial (alive or not)
void nr_tex_verdict(nr_tex_core *c, unsigned nvn_format, int ok);         // the translator took this format as it is (1) or not (0)
// worker: begin (locked) -> run (unlocked) -> end (locked)
int nr_tex_job_begin(nr_tex_core *c, nrt_job *j);     // 1 = a job, its blob bytes reserved; 0 = nothing runnable now (empty / budget)
void nr_tex_job_run(nrt_job *j, nrt_key_fn key);
void nr_tex_job_end(nr_tex_core *c, nrt_job *j, uint64_t now);
// translator: the blob of a live entry whose storage still gives the blob's probe (ownership moves to the caller: nr_tex_drop),
// else NULL and *why. A miss of a queued / running job cancels it (the old path does the upload).
// A prepared object (NR-22) is released here (the caller cannot take it) and NRT_WHY_NONE returned.
uint8_t *nr_tex_take(nr_tex_core *c, uint64_t serial, const void *mem, uint64_t size, uint64_t *bytes, uint64_t *probe, int *why);
void nr_tex_drop(nr_tex_core *c, uint8_t *blob, uint64_t bytes);
// NR-22: nr_tex_take that also hands out the engine's prepared object. 1 = something was handed out: *ts (+ *kind; *blob = its staging
// for NRT_TS_DESW, NULL for NRT_TS_ASTC) or a heap blob (*ts NULL: nr_tex_drop it); the budget (*bytes) is held until nr_tex_unhold /
// nr_tex_drop. 0 = nothing (*why), exactly as nr_tex_take.
int nr_tex_take_ts(nr_tex_core *c, uint64_t serial, const void *mem, uint64_t size, uint8_t **blob, void **ts, int *kind, uint64_t *bytes, uint64_t *probe, int *why);
void nr_tex_unhold(nr_tex_core *c, uint64_t bytes);   // the budget of a taken prepared object back
void nr_tex_set_sink(nr_tex_core *c, const nrt_sink *s);   // NULL = none
int nr_tex_key(nr_tex_core *c, uint64_t serial, const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler);   // 1 = served
void nr_tex_expire(nr_tex_core *c, uint64_t now, uint64_t max_age);   // READY blobs older than max_age are freed
int nr_tex_summary(const nr_tex_core *c, char *out, size_t cap);      // one line, no newline

#ifdef __cplusplus
}
#endif
#endif
