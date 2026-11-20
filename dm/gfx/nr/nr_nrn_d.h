// [DOS2] NR-NATIVE step D (docs/dos2/nr/NR-NATIVE-D.md): the card at LIST CONSTRUCTION. Pure C (C and C++, no NVN / Vulkan / engine
// type, no environment): the front end dm/host/hle_dos2_nrn_d.inc and the host tests dm/gfx/vulkan/engine/tests/nr_nrn_d_test.c /
// nr_nrn_d_tsan_test.cpp include it. main = main.raw of update v786432, build 4979b200.
//
// The engine builds a stage's command list on worker threads: for every object of a draw queue it calls the object's method 23
// "draw yourself" (object, queue entry, context), which appends a GROUP of packets to the list of the context: [0x3a] 21 (all state
// of the draw) 28* (parameters: the object matrix, the material's values) 15* / 17* (textures / samplers) [0x3d] 36 / 37 / 35 (the draw)
// [0x3e]. The list is executed later, on the render thread, packet by packet (ExecuteRenderCommand 0x93e5f0).
//   the note     DM_NRN_LIST: the wrapper of method 23 (hle_dos2_nrn_d.inc) appends ONE packet of its own, opcode NRND_OP, in front
//                of the group (the engine's AllocCommand 0x941720, so it lives in the list, is freed with it and needs no table and
//                no lock), calls the engine's function and then fills the packet from the group the function appended: the pass of
//                the context (flags, mask, the stage's vtable), the address of the group's draw packet and two hashes of the
//                packets of the group - before the draw (lpre) and after it (lpost) - over the fields that decide what the group
//                leaves in the renderer's state cache (nrnd_mix_pkt). Everything is read from the context ARGUMENT: list building
//                is multi-threaded and a list is built by one task.
//   at execution the shared ExecuteRenderCommand hook hands the note to the render thread (the engine never sees the opcode); the
//                draw whose packet address the note names takes from it
//                (1) the PASS: cascade i / pre-pass / G-buffer by the context, not by the class of the names of its targets;
//                (2) the CARD without hashing the 0x180 + 4 x 0x80 bytes of the renderer's cache: the cache after a group is a
//                    function of the cache before it and of the group's packets, so the card of this draw is a function of
//                    (the card of the previous draw, lpost of the previous group, lpre of this one) - nrnd_chain_key - as long as
//                    NOTHING else was executed in between (every other packet breaks the chain: nrnd_run). The memo (nrnd_memo)
//                    keeps key -> card; a miss hashes the cache as before and teaches the memo. The card is the SAME word step B
//                    uses (nrn_card_of: every texture and sampler slot is in it), so rows, images and the publication are shared
//                    and the guarantee of the image - "the card decides every slot the flush re-sends" - is untouched.
//   =2 verify    the cache is hashed at every draw (that card is used); the memo's prediction is compared with it - BAD = the
//                chain did not decide the card (red line) - and the pass of the note with the pass by target class.
//   =1 apply     the memo's card and the note's pass are used; 1 draw in NRND_AUDIT is still hashed and compared (BAD = red
//                line and the memo is switched off for the rest of the run).
// What the chain rests on (docs section 3): the handlers of the group's packets write the card's bytes of the cache as a function of
// the packet fields mixed here and of nothing else but those bytes; a texture packet names a texture RECORD (resolved at list
// construction), so the count of texture creations / destructions is part of the key (nrnd_chain_key epoch).
#ifndef DM_NR_NRN_D_H
#define DM_NR_NRN_D_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nr_nrn.h"

// The mutation switch of the host test (never defined in the engine build): NRND_MUTATE = k - 1 a packet of unknown length does not
// stop the scan (the note stays FULL), 2 the texture packets are not mixed into the hash, 3 a foreign packet does not break the
// chain, 4 the key ignores lpost of the previous group, 5 the note of another draw packet is taken, 6 the chain survives a draw whose
// card was not taken, 7 the key ignores the texture epoch, 8 a list change does not break the chain, 9 a cascade is called the
// pre-pass, 10 the alignment byte is not honoured by the scan, 11 the memo answers a key it does not hold (slot only), 12 the composed
// record lacks the texture-set generation.
#ifndef NRND_MUTATE
#define NRND_MUTATE 0
#endif

// ---- the engine's packets (ExecuteRenderCommand 0x93e5f0: the length each handler returns) ----
enum { NRND_OP = 0x40,          // our packet (the engine has 64 opcodes: 0..0x3f)
       NRND_VER = 1, NRND_NOTE_BYTES = 0x38, NRND_NOTE_ALIGN = 8,
       NRND_PK_STATE = 0x15, NRND_PK_PARAM = 0x1c, NRND_PK_TEX = 0x0f, NRND_PK_SAMP = 0x11,
       NRND_PK_DRAW_ARRAYS = 0x23, NRND_PK_DRAW = 0x24, NRND_PK_DRAW_INST = 0x25 };
// Fixed lengths (hle_dos2_native.c k_len, the same table; 0 = variable or branching). tests/source_structure_test.py compares the two.
static const uint8_t k_nrnd_len[64] = {
    0,    0,    0x20, 0x30, 0x40, 0x20, 0x18, 0x10, 0x18, 0x10, 0x10, 0x18, 0x10, 2,    0x18, 0x20,
    0x18, 0x18, 0x14, 0x14, 0x14, 0x70, 0,    0x98, 0x4c, 0x10, 0x10, 1,    0,    0x20, 0x20, 0x10,
    0x10, 1,    1,    0xc,  0x14, 0x1c, 0x50, 0x90, 0x40, 0x40, 0xa0, 0x10, 0,    0x18, 0,    0x18,
    0x10, 0,    0x10, 0x18, 0x18, 0x28, 0x14, 0,    0xc,  2,    2,    0xc,  0x18, 0x10, 0x10, 0x10};
// Bytes of the packet at p (its opcode byte first). 0 = not known here (the scan of the group stops: no note for it).
static inline uint32_t nrnd_pkt_len(const uint8_t *p) {
    const unsigned op = p[0];
    if (op == NRND_OP) return NRND_NOTE_BYTES;
    if (op >= 64) return 0;
    if (op == NRND_PK_PARAM) { uint32_t n; memcpy(&n, p + 0x28, 4); return n <= 0x10000 ? n + 0x30 : 0; }   // 0x93e5f0 case 0x1c: [pkt+0x28] + 0x30
    return k_nrnd_len[op];
}
// The packet after the cursor at (the engine's list walker 0xac1f80 / AllocCommand 0x941720): the byte at the cursor is the
// alignment a of the next packet, which starts at the next multiple of a AFTER the cursor (a whole a further when the cursor is a
// multiple itself). base: the chunk's data (0x40 aligned, so offsets and addresses align alike). 0 = not a packet start.
static inline uint32_t nrnd_pkt_at(const uint8_t *base, uint32_t at, uint32_t end) {
    if (at >= end) return 0;
    const uint32_t a = base[at];
    if (!a || (a & (a - 1)) || a > 0x40) return 0;
    const uint32_t r = at % a, p = NRND_MUTATE == 10 ? at + 1 : at + (r ? a - r : a);
    return p < end ? p : 0;
}

// ---- what a packet of a group contributes to the hash ----
// Only fields the writers fill (0x90cbf0 / 0xb7e020 leave bytes 1..7, +0x1c, +0x59..0x5f and +0x6c of packet 21 as they were) and
// only fields that decide the cache: packet 21 - shader, raster + its argument, depth-stencil, blend, vertex format, three vertex
// buffers, index buffer, index type, query, alpha reference; 15 - the shader's texture description, the texture record, the word
// after them; 17 - the name and the sampler handle; the two-byte packets - their byte; a parameter packet (28) writes the shader's
// shadow blocks only: nothing. Any other packet is mixed whole by its opcode and length (it is in no group the engine builds today;
// when it appears the hash still separates groups that differ in it).
static inline uint64_t nrnd_mix_pkt(uint64_t h, const uint8_t *p, uint32_t len) {
    const unsigned op = p[0];
    uint64_t w;
    h = nrn_mix(h, 0x706b7400u + op);
    switch (op) {
    case NRND_PK_STATE: {
        static const uint8_t off8[] = {0x08, 0x10, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x60};
        for (unsigned i = 0; i < sizeof off8; i++) { memcpy(&w, p + off8[i], 8); h = nrn_mix(h, w); }
        uint32_t ra, al; memcpy(&ra, p + 0x18, 4); memcpy(&al, p + 0x68, 4);
        return nrn_mix(nrn_mix(h, (uint64_t)ra | (uint64_t)p[0x58] << 32), al); }
    case NRND_PK_TEX: {
        if (NRND_MUTATE == 2) return h;
        uint32_t x; memcpy(&w, p + 8, 8); h = nrn_mix(h, w); memcpy(&w, p + 0x10, 8); h = nrn_mix(h, w); memcpy(&x, p + 0x18, 4);
        return nrn_mix(h, x); }
    case NRND_PK_SAMP: memcpy(&w, p + 8, 8); h = nrn_mix(h, w); memcpy(&w, p + 0x10, 8); return nrn_mix(h, w);
    case NRND_PK_PARAM: return h;
    case 0x39: case 0x3a: case 0x0d: return nrn_mix(h, p[1]);
    case 0x3d: case 0x3e: case 0x3f: memcpy(&w, p + 8, 8); return nrn_mix(nrn_mix(h, w), p[1]);
    default:
        for (uint32_t i = 1; i < len; i++) h = nrn_mix(h, p[i]);
        return h;
    }
}

// ---- the note ----
enum { NRND_F_DONE = 1,     // filled (the engine's function returned and the group was scanned)
       NRND_F_FULL = 2,     // every packet of the group has a known length: lpre / lpost / npkt / draw are the whole group
       NRND_F_DRAW = 4,     // exactly one draw packet 36 / 37 in the group: `draw` is its address
       NRND_F_ONE = 8 };    // [DOS2] NR-NATIVE step D half c (DM_NRN_PKT=1, nr_pkt.h): the group is the payload of this packet - the chunk does not count its packets
// kind: which function was wrapped (NRND_K_*: the report's breakdown). cflags / cmask: context +8 / +0xc. stage: the vtable of the
// stage in the context (+0x18), as an offset in main (0 = none / not in main). sub: the cascade (stage +0xb8 + 2 x stage +0xb0) of
// a cascade stage. npkt: packets of the group (the note not counted).
typedef struct nrnd_note {
    uint8_t op, ver, flags, kind; uint32_t cflags; uint16_t cmask, sub; uint32_t npkt;
    uint64_t draw, lpre, lpost; uint32_t stage, npre; uint64_t obj;
} nrnd_note;
#ifdef __cplusplus
static_assert(sizeof(nrnd_note) == NRND_NOTE_BYTES, "the note packet");
#else
_Static_assert(sizeof(nrnd_note) == NRND_NOTE_BYTES, "the note packet");
#endif
enum { NRND_K_COMMON, NRND_K_INST, NRND_K_B89330, NRND_K_B87470, NRND_K_C6CC50, NRND_K_C7F590, NRND_K_C82C80, NRND_K_N };
// One contiguous piece of list memory the group lies in (a group that crosses chunks has several).
typedef struct nrnd_seg { const uint8_t *base; uint32_t at, end; } nrnd_seg;
// Scans the packets the wrapped function appended (segments in list order; the first starts right after the note) and fills the
// hashes, the counts and the draw packet of the note. A packet of unknown length, a note of a nested call or a second draw packet
// ends it without NRND_F_FULL / NRND_F_DRAW (such a note gives the pass only).
static inline void nrnd_scan(nrnd_note *n, const nrnd_seg *seg, unsigned nseg) {
    uint64_t pre = 0x6e726e645f707265ull, post = 0x6e726e645f706f73ull, draw = 0;
    uint32_t npkt = 0, npre = 0; int full = 1, draws = 0;
    for (unsigned s = 0; s < nseg && full; s++) {
        const uint8_t *b = seg[s].base; uint32_t at = seg[s].at; const uint32_t end = seg[s].end;
        while (at < end) {
            const uint32_t p = nrnd_pkt_at(b, at, end);
            if (!p) { full = 0; break; }
            const uint32_t len = nrnd_pkt_len(b + p);
            if (!len || p + len > end || b[p] == NRND_OP) { if (NRND_MUTATE != 1 || b[p] == NRND_OP) full = 0; at = end; break; }
            const unsigned op = b[p];
            if (op == NRND_PK_DRAW || op == NRND_PK_DRAW_INST) { if (!draws++) draw = (uint64_t)(uintptr_t)(b + p); }
            else if (op == NRND_PK_DRAW_ARRAYS) draws += 2;   // DrawArrays is not recorded directly: no card is taken at it
            else if (!draws) { pre = nrnd_mix_pkt(pre, b + p, len); npre++; }
            else post = nrnd_mix_pkt(post, b + p, len);
            npkt++; at = p + len;
        }
    }
    n->npkt = npkt; n->npre = npre; n->lpre = pre; n->lpost = post; n->draw = draws == 1 ? draw : 0;
    n->flags = (uint8_t)(NRND_F_DONE | (full ? NRND_F_FULL : 0) | (full && draws == 1 ? NRND_F_DRAW : 0));
}
static inline void nrnd_note_init(nrnd_note *n, int kind, uint32_t cflags, uint32_t cmask, uint32_t stage, unsigned sub, uint64_t obj) {
    memset(n, 0, sizeof *n);
    n->op = NRND_OP; n->ver = NRND_VER; n->kind = (uint8_t)kind; n->cflags = cflags; n->cmask = (uint16_t)cmask; n->stage = stage; n->sub = (uint16_t)sub; n->obj = obj;
}

// ---- the pass of a context (NR-NATIVE-RE.md section 0: flags and mask per pass, the stage that owns the list) ----
enum { NRND_VT_CASCADE = 0x299b0a0, NRND_VT_GBUF_STAGE = 0x299b5b8 };
// NRN_P_* or -1 = the context does not say (reflections 0x20002, local-light shadows, forward queues: the class of the targets decides).
static inline int nrnd_pass_of(uint32_t cflags, uint32_t stage) {
    if (cflags == 2 && stage == NRND_VT_CASCADE) return NRND_MUTATE == 9 ? NRN_P_PREPASS : NRN_P_SHADOW;
    if (cflags == 0x40000 && stage == NRND_VT_GBUF_STAGE) return NRN_P_PREPASS;
    if (cflags == 1 && stage == NRND_VT_GBUF_STAGE) return NRN_P_GBUF;
    return -1;
}

// ---- the chain on the render thread ----
// run: what the thread that executes lists knows between two packets. remain: packets of the current group still to come (a packet
// that arrives with remain == 0 belongs to no noted group: foreign). chain: prev is the card of the cache as the last executed
// packet left it, up to lpost (the packets of the previous group after its draw).
typedef struct nrnd_run {
    const nrnd_note *note; uint64_t list, cur_draw, prev, lpost;
    uint32_t remain; uint8_t chain, took, pad[2];
} nrnd_run;
// The note packet is executed (list: the list being walked).
static inline void nrnd_run_note(nrnd_run *r, uint64_t list, const nrnd_note *n) {
    if ((list != r->list && NRND_MUTATE != 8) || r->remain || !(n->flags & NRND_F_DONE) || n->ver != NRND_VER) r->chain = 0;
    r->list = list; r->note = NULL; r->remain = 0;
    if (!(n->flags & NRND_F_DONE) || n->ver != NRND_VER) return;
    r->note = n;
    if (n->flags & NRND_F_FULL) r->remain = n->npkt; else r->chain = 0;
}
// Any other packet is about to be executed.
static inline void nrnd_run_pkt(nrnd_run *r, unsigned op, uint64_t pkt) {
    if (r->remain) r->remain--;
    else if (NRND_MUTATE != 3) r->chain = 0;
    if (op == NRND_PK_DRAW || op == NRND_PK_DRAW_INST) {
        if (!r->took && r->cur_draw && NRND_MUTATE != 6) r->chain = 0;   // the previous draw packet never came for its card
        r->cur_draw = pkt; r->took = 0;
    } else if (op == NRND_PK_DRAW_ARRAYS) r->chain = 0;
}
// The note of the draw whose packet is being executed (NULL: none - an unwrapped class, a nested group, a scan that stopped).
static inline const nrnd_note *nrnd_run_take(nrnd_run *r) {
    const nrnd_note *n = r->note;
    r->took = 1;
    if (!n || !(n->flags & NRND_F_DRAW)) return NULL;
    if (n->draw != r->cur_draw && NRND_MUTATE != 5) return NULL;
    return n;
}
// The key of the card of the draw of note n: 0 = the chain does not hold (hash the cache). epoch: texture creations + destructions.
static inline uint64_t nrnd_chain_key(const nrnd_run *r, const nrnd_note *n, uint64_t epoch) {
    if (!n || !r->chain || (n->flags & (NRND_F_FULL | NRND_F_DRAW)) != (NRND_F_FULL | NRND_F_DRAW)) return 0;
    uint64_t k = nrn_mix(r->prev, NRND_MUTATE == 4 ? 0 : r->lpost);
    k = nrn_mix(nrn_mix(k, n->lpre), NRND_MUTATE == 7 ? 0 : epoch);
    return k ? k : 1;
}
// After the card of the draw was taken (card: nrn_card_of of the cache, or the memo's): the chain goes on from it when the note was
// whole, and stops otherwise (card 0: no card was taken - a pass that is off).
static inline void nrnd_run_drawn(nrnd_run *r, const nrnd_note *n, uint64_t card) {
    r->note = NULL;
    if (n && card && (n->flags & (NRND_F_FULL | NRND_F_DRAW)) == (NRND_F_FULL | NRND_F_DRAW)) { r->prev = card; r->lpost = n->lpost; r->chain = 1; }
    else r->chain = 0;
}

// ---- key -> card (the render thread only) ----
enum { NRND_MEMO = 32768 };
typedef struct nrnd_memo { uint64_t key[NRND_MEMO], card[NRND_MEMO]; } nrnd_memo;
static inline uint32_t nrnd_memo_slot(uint64_t key) { return (uint32_t)(key ^ key >> 29) & (NRND_MEMO - 1); }
static inline uint64_t nrnd_memo_get(const nrnd_memo *m, uint64_t key) {   // 0 = not held
    const uint32_t i = nrnd_memo_slot(key);
    return key && (m->key[i] == key || NRND_MUTATE == 11) ? m->card[i] : 0;
}
static inline void nrnd_memo_put(nrnd_memo *m, uint64_t key, uint64_t card) { if (key) { const uint32_t i = nrnd_memo_slot(key); m->key[i] = key; m->card[i] = card; } }

// ---- D-b: the record of a card draw composed directly (the bytes nbd_pack + nrd_tail_put + nrn_tail_put + nrn_short_ext_put +
// nrd_tail_ext_put write; the layouts are those of nvn_nb_draw.h, repeated here as offsets so that this header stays free of it and
// the host test can hold the two against each other) ----
enum { NRND_R_HDR = 64, NRND_R_BIND = 24, NRND_R_NRD = 16 };
typedef struct nrnd_rec_in {
    uint8_t engine_op, draw, flags; uint8_t cond_mode; uint16_t nbind; uint32_t texgen;
    int32_t prim, itype, count, base_vertex, base_instance, instances, first;
    uint64_t ibuf, cond_addr;
    const uint8_t *binds;                                  // nbind x 24 bytes, as the shadow holds them
    uint32_t nr_key, nr_texset; uint16_t nr_why; uint8_t nr_site; uint32_t nr_ext;   // the NR-09 tail (+ NR-09R's word)
    uint64_t card; uint8_t pass; uint16_t rsv; uint32_t seq;                          // the NR-NATIVE tail
    const nrn_short_ext *sx;                               // the short ext (NULL: a long record)
} nrnd_rec_in;
static inline uint32_t nrnd_rec_size(const nrnd_rec_in *in) {
    return (uint32_t)(NRND_R_HDR + NRND_R_BIND * (uint32_t)in->nbind + NRND_R_NRD + NRN_TAIL_BYTES + (in->sx ? NRN_SHORT_EXT_BYTES : 0));
}
// Writes the payload (cap >= nrnd_rec_size, zeroed by the caller as rec() zeroes a record). Returns the bytes written, 0 = no room.
static inline uint32_t nrnd_rec_put(uint8_t *dst, size_t cap, const nrnd_rec_in *in) {
    const uint32_t n = nrnd_rec_size(in);
    if (!dst || cap < n || in->nbind > 64) return 0;
    uint8_t *p = dst;
    p[0] = 1 /* NBD_VER */; p[1] = in->engine_op; p[2] = in->draw; p[3] = (uint8_t)(in->flags | 16 /* NBD_F_NR */ | NRN_F_FLAG);
    memcpy(p + 4, &in->nbind, 2); p[6] = in->cond_mode; p[7] = 0;
    if (NRND_MUTATE != 12) memcpy(p + 8, &in->texgen, 4);
    memset(p + 12, 0, 4);
    const int32_t a[8] = {in->prim, in->itype, in->count, in->base_vertex, in->base_instance, in->instances, in->first, 0};
    memcpy(p + 16, a, 32); memcpy(p + 48, &in->ibuf, 8); memcpy(p + 56, &in->cond_addr, 8);
    if (in->nbind) memcpy(p + NRND_R_HDR, in->binds, (size_t)NRND_R_BIND * in->nbind);
    uint8_t *t = p + NRND_R_HDR + NRND_R_BIND * (uint32_t)in->nbind;
    memcpy(t, &in->nr_key, 4); memcpy(t + 4, &in->nr_texset, 4); memcpy(t + 8, &in->nr_why, 2); t[10] = 1 /* NRD_VER */; t[11] = in->nr_site; memcpy(t + 12, &in->nr_ext, 4);
    nrn_tail_put(dst, cap, (size_t)(t - p) + NRND_R_NRD, in->card, in->pass, in->seq, in->rsv);
    if (in->sx) nrn_short_ext_put(dst, cap, (size_t)(t - p) + NRND_R_NRD + NRN_TAIL_BYTES, in->sx);
    return n;
}
#endif
