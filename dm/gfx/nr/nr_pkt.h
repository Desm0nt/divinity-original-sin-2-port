// [DOS2] NR-NATIVE step D, half c (DM_NRN_PKT, default 0; docs/dos2/nr/NR-NATIVE-D.md section 13): the packets of a draw's group that
// only touch the renderer's own memory, as native code. Pure C (no NVN / Vulkan / engine type, no environment): the front end
// dm/host/hle_dos2_nrn_c.inc and the host test dm/gfx/vulkan/engine/tests/nr_pkt_test.c include it; tests/nr_pkt_oracle.py runs the
// engine's own machine code (ExecuteRenderCommand 0x93e5f0 with these opcodes, from main.raw) on the same states and compares every
// byte. main = main.raw of update v786432, build 4979b200.
//   opcode 0x0f (0x20 bytes)   texture: 0x93e5f0 calls 0x9243d0(r, pkt + 8). pkt+8 the texture's description in the shader record
//                              (i8 slot per stage at +0 / +1 / +2, i32 sampler at +4), pkt+0x10 the address of the texture record
//                              pointer, pkt+0x18 i32 view (-1 none). The texture id is [[rec]+0xc0], or the view's
//                              [[rec]+0xc0+4(view+1)] when that is not -1; the sampler id is the description's, when negative
//                              [[rec]+0x100], when negative the renderer's default sampler (table r+0x540 / r+0x550, index = the
//                              global main+0x2bbda90). For each stage with a slot >= 0: the texture table and then the sampler
//                              table - a different id is stored and the table's dirty range (i8 min at +0x84, i8 max at +0x85)
//                              is widened to the slot.
//   opcode 0x1c (0x30 + n)     parameter: 0x93e5f0 calls 0x924bb0(pkt + 8, pkt + 0x30, n = [pkt+0x28]). pkt+8 / +0x10 / +0x18 the
//                              shader's shadow block of each stage (0 none), pkt+0x20 / +0x22 / +0x24 u16 offset in it (0xffff
//                              none). Per stage: block+0x28 (u32, bytes in use) = max(it, offset + n); when the n bytes at
//                              [block+0x20] + offset differ from the values, block+0x34 (u8 dirty) = 1 and they are copied.
//   opcodes 0x39 / 0x3a (2)    nothing but their length. Opcode 0x11 (0x18): 0x801dd0 on a zeroed local - nothing (the oracle
//                              holds both: no byte outside the stack is written).
// Every store goes through nrpk_put: with a journal the model stores nothing and lists what it would store (DM_NRN_PKT=2: the
// engine's handler runs next and every listed byte must be what memory then holds).
#ifndef DM_NR_PKT_H
#define DM_NR_PKT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nr_nrn_d.h"
#ifndef NRPK_PTR
#define NRPK_PTR(a) ((uint8_t *)(uintptr_t)(a))   // guest address -> host pointer (the identity in the port; the tests map an arena)
#endif
#ifndef NRPK_MUTATE
#define NRPK_MUTATE 0   // host test only: 1 the view's id is not taken, 2 the description's sampler wins even when negative, 3 the dirty range is not widened, 4 a stage's sampler table is skipped, 5 the bytes in use are not raised, 6 the dirty byte is not set, 7 the third stage is skipped, 8 the values are copied without the compare
#endif
enum { NRPK_OP_TEX = 0x0f, NRPK_OP_SAMP = 0x11, NRPK_OP_PARAM = 0x1c, NRPK_OP_2A = 0x39, NRPK_OP_2B = 0x3a,
       NRPK_GDEF = 0x2bbda90,     // main + this: u32 index of the default sampler
       NRPK_R_SMP_PAGES = 0x540, NRPK_R_SMP_PER = 0x550, NRPK_PARAM_MAX = 0x10000,
       NRPK_J_MAX = 16, NRPK_J_BYTES = 8192 };
static const uint16_t k_nrpk_tex[3] = {0x1558, 0x15e0, 0x1668}, k_nrpk_smp[3] = {0x13c0, 0x1448, 0x14d0};   // 32 x i32 ids; +0x84 / +0x85 the dirty range
typedef struct { uint64_t addr; uint32_t n, off; } nrpk_jent;
// over: something the journal cannot stand for (no room, a slot past the table, two stages on one block): the packet is not compared.
typedef struct { uint32_t n, used, over; nrpk_jent e[NRPK_J_MAX]; uint8_t b[NRPK_J_BYTES]; } nrpk_journal;
static inline uint64_t nrpk_r64(uint64_t a) { uint64_t v; memcpy(&v, NRPK_PTR(a), 8); return v; }
static inline uint32_t nrpk_r32(uint64_t a) { uint32_t v; memcpy(&v, NRPK_PTR(a), 4); return v; }
static inline uint16_t nrpk_r16(uint64_t a) { uint16_t v; memcpy(&v, NRPK_PTR(a), 2); return v; }
static inline void nrpk_put(nrpk_journal *j, uint64_t addr, const void *src, uint32_t n) {
    if (!j) { memmove(NRPK_PTR(addr), src, n); return; }
    if (j->n >= NRPK_J_MAX || n > NRPK_J_BYTES - j->used) { j->over = 1; return; }
    j->e[j->n].addr = addr; j->e[j->n].n = n; j->e[j->n].off = j->used; j->n++;
    memcpy(j->b + j->used, src, n); j->used += n;
}
// 1 = every byte the journal lists is what memory holds now.
static inline int nrpk_journal_holds(const nrpk_journal *j, uint64_t *addr) {
    for (uint32_t i = 0; i < j->n; i++) if (memcmp(NRPK_PTR(j->e[i].addr), j->b + j->e[i].off, j->e[i].n)) { if (addr) *addr = j->e[i].addr; return 0; }
    return 1;
}
// One table of a stage: the id of slot c.
static inline void nrpk_slot(nrpk_journal *j, uint64_t r, uint32_t tab, int c, int32_t v) {
    if ((int32_t)nrpk_r32(r + tab + 4 * (uint64_t)c) == v) return;
    if (j && c >= 32) j->over = 1;   // the store would land in the range bytes / the next table: later loads of this packet read it
    nrpk_put(j, r + tab + 4 * (uint64_t)c, &v, 4);
    if (NRPK_MUTATE == 3) return;
    const int8_t mx = (int8_t)*NRPK_PTR(r + tab + 0x85), nmx = (int8_t)c <= mx ? mx : (int8_t)c;
    nrpk_put(j, r + tab + 0x85, &nmx, 1);
    const int8_t mn = (int8_t)*NRPK_PTR(r + tab + 0x84), nmn = mn <= (int8_t)c ? mn : (int8_t)c;
    nrpk_put(j, r + tab + 0x84, &nmn, 1);
}
// Opcode 0x0f: 0x9243d0(r, d = pkt + 8). gdef = the u32 at main + NRPK_GDEF.
static inline void nrpk_p15(nrpk_journal *j, uint64_t r, uint64_t d, uint32_t gdef) {
    const uint64_t desc = nrpk_r64(d);
    if (!desc) return;
    const uint64_t recp = nrpk_r64(d + 8);
    if (!recp) return;
    const uint64_t t = nrpk_r64(recp);
    int32_t tex = (int32_t)nrpk_r32(t + 0xc0);
    const int32_t view = (int32_t)nrpk_r32(d + 0x10);
    if (view != -1 && NRPK_MUTATE != 1) { const int32_t v = (int32_t)nrpk_r32(t + (uint64_t)((int64_t)(view + 1) * 4) + 0xc0); if (v != -1) tex = v; }
    int32_t smp = (int32_t)nrpk_r32(desc + 4);
    if (smp < 0 && NRPK_MUTATE != 2) {
        smp = (int32_t)nrpk_r32(t + 0x100);
        if (smp < 0) {
            const uint32_t per = nrpk_r32(r + NRPK_R_SMP_PER), q = per ? gdef / per : 0;   // udiv by 0 = 0 on AArch64
            smp = (int32_t)nrpk_r32(nrpk_r64(nrpk_r64(r + NRPK_R_SMP_PAGES) + (uint64_t)q * 8) + (uint64_t)(gdef - q * per) * 0x20 + 0x10);
        }
    }
    for (int k = 0; k < 3; k++) {
        const int c = (int8_t)*NRPK_PTR(desc + (uint64_t)k);
        if (c < 0) continue;
        nrpk_slot(j, r, k_nrpk_tex[k], c, tex);
        if (NRPK_MUTATE == 4 && k == 1) continue;
        nrpk_slot(j, r, k_nrpk_smp[k], c, smp);
    }
}
// Opcode 0x1c: 0x924bb0(pkt + 8, pkt + 0x30, [pkt + 0x28]). Returns the packet's length.
static inline uint32_t nrpk_p28(nrpk_journal *j, uint64_t pkt) {
    const uint32_t n = nrpk_r32(pkt + 0x28);
    const uint64_t blk[3] = {nrpk_r64(pkt + 8), nrpk_r64(pkt + 0x10), nrpk_r64(pkt + 0x18)};
    if (j && ((blk[0] && (blk[0] == blk[1] || blk[0] == blk[2])) || (blk[1] && blk[1] == blk[2]))) j->over = 1;   // a later stage would read what an earlier one stored
    for (int k = 0; k < (NRPK_MUTATE == 7 ? 2 : 3); k++) {
        const uint32_t off = nrpk_r16(pkt + 0x20 + 2 * (uint64_t)k);
        if (!blk[k] || off == 0xffff) continue;
        const uint32_t used = nrpk_r32(blk[k] + 0x28), end = off + n, now = end <= used || NRPK_MUTATE == 5 ? used : end;
        nrpk_put(j, blk[k] + 0x28, &now, 4);
        const uint64_t dst = nrpk_r64(blk[k] + 0x20) + off;
        if (NRPK_MUTATE != 8 && !memcmp(NRPK_PTR(dst), NRPK_PTR(pkt + 0x30), n)) continue;
        if (NRPK_MUTATE != 6) { const uint8_t one = 1; nrpk_put(j, blk[k] + 0x34, &one, 1); }
        nrpk_put(j, dst, NRPK_PTR(pkt + 0x30), n);
    }
    return n + 0x30;
}
// One packet of the kinds above at pkt, for the renderer r. 0 = not one of them (nothing done); else its length.
static inline uint32_t nrpk_exec(nrpk_journal *j, uint64_t r, uint64_t pkt, uint32_t gdef) {
    switch (*NRPK_PTR(pkt)) {
    case NRPK_OP_TEX: nrpk_p15(j, r, pkt + 8, gdef); return 0x20;
    case NRPK_OP_PARAM: return nrpk_r32(pkt + 0x28) <= NRPK_PARAM_MAX ? nrpk_p28(j, pkt) : 0;
    case NRPK_OP_SAMP: return 0x18;
    case NRPK_OP_2A: case NRPK_OP_2B: return 2;
    default: return 0;
    }
}
// ---- one packet instead of the group ----
// The engine's list walkers (0xa49c30, 0xac1f80, 0xb15020: one inlined template) take a chunk's packet COUNT (chunk +0x10) and, per
// packet, the alignment byte at the cursor, the packet at the next multiple of it, and the length ExecuteRenderCommand returns. So a
// group that lies in the note's own chunk becomes the note's payload when the chunk's count is lowered by the group's packets and
// the note's handler returns the bytes from the note to the end of the group: the walker lands on the alignment byte of the packet
// after the group, exactly where it would have arrived packet by packet.
// The packet after the cursor (0 = no alignment byte there).
static inline uint64_t nrpk_next(uint64_t cur) {
    const uint64_t a = *NRPK_PTR(cur);
    if (!a) return 0;
    const uint64_t r = cur % a;
    return NRPK_MUTATE == 10 ? cur + 1 : cur + (r ? a - r : a);
}
// The builder's decision, after the group was scanned: every length known (full), the group in the note's chunk only (nseg == 1), and
// the chunk counted exactly the group's packets since the note was put (count_note: the count right after the note's AllocCommand).
static inline int nrpk_merge_ok(int full, unsigned nseg, uint32_t npkt, uint32_t count_note, uint32_t count_now) {
    return full && (nseg == 1 || NRPK_MUTATE == 9) && npkt && count_now == count_note + npkt;
}
// The note's handler for a merged group: its npkt packets in order through f (the same per-packet path the walker's call takes;
// returns the packet's length). Returns the length of the merged packet. *bad: a handler returned 0 (it cannot: the lengths were
// known at construction) - the table's length is taken for that packet and the walk goes on.
typedef uint64_t (*nrpk_pkt_fn)(void *u, unsigned op, uint64_t pkt);
static inline uint64_t nrpk_group_run(uint64_t note, uint32_t npkt, nrpk_pkt_fn f, void *u, int *bad) {
    uint64_t cur = note + NRND_NOTE_BYTES;
    for (uint32_t i = 0; i < npkt; i++) {
        const uint64_t p = nrpk_next(cur);
        if (!p) { *bad = 1; break; }
        uint64_t len = f(u, *NRPK_PTR(p), p);
        if (!len) { *bad = 1; len = nrnd_pkt_len(NRPK_PTR(p)); if (!len) break; }
        cur = p + len;
    }
    return cur - note;
}
// The code the models stand for (FNV-1a 64 of main + off .. end): checked by the front end before DM_NRN_PKT arms and by the oracle.
typedef struct { uint32_t off, end; uint64_t fnv; const char *name; } nrpk_code;
static inline uint64_t nrpk_fnv(const uint8_t *p, size_t n) { uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull; return h; }
#endif
