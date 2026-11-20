// [DOS2] NR-PRESKIN DM_NR_PRESKIN (docs/dos2/nr/NR-PRESKIN.md): the record table of one submission, pure C (host test
// dm/gfx/vulkan/engine/tests/nr_preskin_core_test.c). A record = the pre-skinned words of every vertex of one mesh instance for one set of
// cut values (xbg_spirv_preskin.h): m 32-bit words a vertex at arena offset off, nverts vertices. Each value carries its structural hash and
// the identities of its leaves at the time the slice ran (the attribute / vertex stream of every Location it reads, the bound range of every
// uniform slot it reads). A draw's consumer may read a record iff every value it needs is there with the same hash and the same leaf
// identities as the draw binds now, and the record covers the draw's vertices: then the words are the ones its own slice would store (the
// same instructions on the same inputs - tools/dos2_preskin_check.py proves the shared-hash case on the pack).
#pragma once
#include <stdint.h>
#include <string.h>

#define NRPS_MAXV 16      // values of a record (= at most 16 words a vertex)
#define NRPS_SLOTS 16     // NVN uniform slots of the vertex stage

typedef struct { uint64_t hash; uint32_t first, comps, loc_mask, slot_mask; } nrps_val;   // slot_mask: NVN uniform slots it reads
typedef struct {
    uint64_t bucket;                 // the bone block's identity (the instance)
    uint64_t loc_id[16];             // per Location: identity of its attribute + stream at the slice (0 = the slice did not read it)
    uint64_t slot_id[NRPS_SLOTS];    // per uniform slot: identity of the bound range at the slice (0 = not read)
    uint64_t off;                    // arena offset of word 0 of vertex 0
    uint32_t m, nverts, nvals, uses;
    nrps_val v[NRPS_MAXV];
} nrps_rec;

// identities: never 0 (0 = "not read")
static inline uint64_t nrps_mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }
static inline uint64_t nrps_id_attr(uint32_t vk_format, uint32_t offset, uint32_t stride, uint64_t vb_addr, uint64_t vb_size, uint64_t buf, uint64_t buf_off) {
    uint64_t h = 0xA77Bull; h = nrps_mix(h, vk_format); h = nrps_mix(h, offset); h = nrps_mix(h, stride); h = nrps_mix(h, vb_addr); h = nrps_mix(h, vb_size);
    h = nrps_mix(h, buf); h = nrps_mix(h, buf_off); return h ? h : 1; }
static inline uint64_t nrps_id_ubo(uint64_t addr, uint64_t copy, uint64_t buf, uint64_t buf_off) {
    uint64_t h = 0xB0Bull; h = nrps_mix(h, addr); h = nrps_mix(h, copy); h = nrps_mix(h, buf); h = nrps_mix(h, buf_off); return h ? h : 1; }

// Does r hold every value of want[0..nwant) under the current leaves (cur_loc / cur_slot) for nverts vertices? pos receives, per component of
// want in its own order (want[i].first + j), the record word. 1 = yes.
static inline int nrps_match(const nrps_rec *r, const nrps_val *want, uint32_t nwant, const uint64_t cur_loc[16], const uint64_t cur_slot[NRPS_SLOTS],
                             uint32_t nverts, uint32_t *pos) {
    if (r->nverts < nverts) return 0;
    for (uint32_t i = 0; i < nwant; i++) {
        const nrps_val *w = &want[i]; const nrps_val *hit = 0;
        for (uint32_t k = 0; k < r->nvals; k++) if (r->v[k].hash == w->hash && r->v[k].comps == w->comps) { hit = &r->v[k]; break; }
        if (!hit) return 0;
        for (uint32_t l = 0; l < 16; l++) if ((w->loc_mask >> l & 1) && (!r->loc_id[l] || r->loc_id[l] != cur_loc[l])) return 0;
        for (uint32_t s = 0; s < NRPS_SLOTS; s++) if ((w->slot_mask >> s & 1) && (!r->slot_id[s] || r->slot_id[s] != cur_slot[s])) return 0;
        for (uint32_t j = 0; j < w->comps; j++) pos[w->first + j] = hit->first + j;
    }
    return 1;
}
// pos is the identity 0 .. m-1 (the draw reads its own record's layout)
static inline int nrps_pos_identity(const uint32_t *pos, uint32_t m) { for (uint32_t j = 0; j < m; j++) if (pos[j] != j) return 0; return 1; }
static inline uint64_t nrps_pos_hash(const uint32_t *pos, uint32_t m, uint32_t rec_m) { uint64_t h = nrps_mix(0x9051ull, rec_m); for (uint32_t j = 0; j < m; j++) h = nrps_mix(h, pos[j]); return h; }

// one submission's arena: bump allocation, reset when the slot records again (its previous submission retired)
typedef struct { uint64_t size, used, peak; } nrps_bump;
static inline uint64_t nrps_take(nrps_bump *b, uint64_t bytes, uint64_t align) {
    if (!align) align = 1;
    const uint64_t at = (b->used + align - 1) / align * align;
    if (at > b->size || bytes > b->size - at) return ~0ull;
    b->used = at + bytes; if (b->used > b->peak) b->peak = b->used; return at;
}
// vertices a vertex stream range holds for an attribute (format size fsz at offset off, stride): those whose bytes fit in [0, bytes)
static inline uint32_t nrps_nverts(uint64_t bytes, uint32_t stride, uint32_t off, uint32_t fsz) {
    if (!stride || !fsz || (uint64_t)off + fsz > bytes) return 0;
    const uint64_t n = (bytes - off - fsz) / stride + 1; return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}
