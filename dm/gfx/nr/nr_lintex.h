// [DOS2] NR-TRAFFIC DM_NR_LINTEX (docs/dos2/nr/NR-TRAFFIC.md): re-upload only the rows of a pitch-linear texture the CPU wrote.
// Pure C (C and C++): no Vulkan, no game addresses. The backend (xbg_nr_lintex.inc, namespace nrlin) asks the dirty tracker
// (dm/gfx/nvn/nvn_dirty.c) through the callback; dm/gfx/vulkan/engine/tests/nr_lintex_test.c tests it on the PC.
//
// Why: DOS2's fog-of-war / map texture is a 1424x1602 RGBA8 pitch-linear texture (NVN flags 0x10, 9 176 256 bytes) the game writes
// from the CPU while it is sampled (logs: "NVN big texture hash (untracked) ... flags=0x10 size=9176256 stable=0 partial_why=0x20").
// Pitch-linear textures are excluded from the partial (GOB-block) re-upload, so every change costs a 9 MB hash at the check, a full
// row copy into a zero-filled vector, a second copy into the staging arena and a second 9 MB hash after the upload (~45 MB of CPU
// memory traffic; long-frame lines "重传 1 次 8.7 MB / 2.5 ... 12.8 ms", up to 3 a frame in combat).
// The rows of a pitch-linear texture are plain byte ranges (row y = [y * pitch, y * pitch + row_bytes)), so the page tracker's
// answer maps onto rows directly: a row is re-uploaded when a page it touches was written since the stamp.
#ifndef DM_NR_LINTEX_H
#define DM_NR_LINTEX_H
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRLT_OFF = 0, NRLT_APPLY = 1, NRLT_VERIFY = 2, NRLT_MODES };

typedef struct { uint32_t y0, n; } nrlt_run;   // rows [y0, y0 + n)

// Row pitch of a pitch-linear NVN texture as the engine's full upload reads it (xbg_game.cpp get_image_slow, flags 0x10): the
// descriptor's stride, else the row rounded up to 32 bytes.
static inline uint64_t nrlt_pitch(uint32_t stride, uint32_t width, uint32_t bpp) {
    return stride ? (uint64_t)stride : (((uint64_t)width * bpp + 31u) & ~(uint64_t)31u);
}
// 1 when h rows of row_bytes at pitch fit in size bytes of storage (the last row needs only row_bytes).
static inline int nrlt_fits(uint64_t size, uint32_t h, uint64_t pitch, uint64_t row_bytes) {
    if (!h || !row_bytes || pitch < row_bytes) return 0;
    const uint64_t last = (uint64_t)(h - 1) * pitch;
    return last / pitch == (uint64_t)(h - 1) && last + row_bytes >= last && last + row_bytes <= size;
}

// The runs of rows to copy. dirty(ctx, off, len) != 0 when a byte of [off, off + len) (offsets in the texture storage) may have been
// written since the stamp; it may answer 1 too often (whole pages), never 0 for a written byte. Rows are probed in groups of `group`
// first (one call for a clean group), then one by one. A clean row between two dirty rows that are at most `gap` rows apart is copied
// with them (its bytes are the uploaded ones: copying it again is harmless and saves a copy region); at `cap` runs the last run is
// extended instead of opening a new one. Returns the run count; *dirty_rows = rows the tracker reported (without the gap fill).
static inline uint32_t nrlt_runs(uint32_t h, uint64_t pitch, uint64_t row_bytes, int (*dirty)(void *ctx, uint64_t off, uint64_t len),
                                 void *ctx, uint32_t group, uint32_t gap, nrlt_run *out, uint32_t cap, uint32_t *dirty_rows) {
    uint32_t n = 0, d = 0;
    if (!group) group = 1;
    for (uint32_t g0 = 0; g0 < h; g0 += group) {
        const uint32_t g1 = h - g0 > group ? g0 + group : h;
        if (group > 1 && !dirty(ctx, (uint64_t)g0 * pitch, (uint64_t)(g1 - 1 - g0) * pitch + row_bytes)) continue;
        for (uint32_t y = g0; y < g1; y++) {
            if (!dirty(ctx, (uint64_t)y * pitch, row_bytes)) continue;
            d++;
            if (n) {
                nrlt_run *r = &out[n - 1];
                const uint32_t end = r->y0 + r->n;
                if (y < end + gap + 1 || n == cap) { r->n = y + 1 - r->y0; continue; }
            }
            if (!cap) continue;
            out[n].y0 = y; out[n].n = 1; n++;
        }
    }
    if (dirty_rows) *dirty_rows = d;
    return n;
}
// Rows covered by the runs.
static inline uint64_t nrlt_rows(const nrlt_run *r, uint32_t n) { uint64_t s = 0; for (uint32_t i = 0; i < n; i++) s += r[i].n; return s; }

// Verify mode: `was` holds the rows as they were when the stamp was taken (tight, row_bytes each), `now` the storage (at pitch).
// Returns the rows whose bytes changed but lie in no run (a write the tracker did not report: must be 0); *changed = rows that changed.
static inline uint32_t nrlt_missed(const uint8_t *was, const uint8_t *now, uint64_t pitch, uint32_t h, uint64_t row_bytes,
                                   const nrlt_run *runs, uint32_t n, uint32_t *changed) {
    uint32_t miss = 0, ch = 0, k = 0;
    for (uint32_t y = 0; y < h; y++) {
        if (!memcmp(was + (uint64_t)y * row_bytes, now + (uint64_t)y * pitch, (size_t)row_bytes)) continue;
        ch++;
        while (k < n && runs[k].y0 + runs[k].n <= y) k++;
        if (!(k < n && runs[k].y0 <= y)) miss++;
    }
    if (changed) *changed = ch;
    return miss;
}

// Give-up rule (mode 1): an update that dirties more than pct % of the rows is dense; `streak` dense updates in a row return 1
// (the texture goes back to the old path: hash + whole re-upload, no write protection - a texture rewritten whole would pay one
// write fault per page on the game thread, more than the hash costs). A sparse update resets the streak.
typedef struct { uint32_t dense; } nrlt_policy;
static inline int nrlt_note(nrlt_policy *p, uint64_t dirty_rows, uint32_t h, uint32_t pct, uint32_t streak) {
    if (h && dirty_rows * 100u > (uint64_t)h * pct) { if (++p->dense >= streak) return 1; }
    else p->dense = 0;
    return 0;
}

#ifdef __cplusplus
}
#endif
#endif
