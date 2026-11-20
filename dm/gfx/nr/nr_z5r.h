// [DOS2] NR-ZERO Z5, the run protocol with a verdict per draw (DM_NR_Z5, docs/dos2/nr/NR-ZERO.md section 22). C++, no Vulkan / engine
// type: the run assembler is nr_z5.h, the queue nb_mt.h, the game thread's values nr_z5p.h; the engine side is xbg_nr_z5.inc
// (namespaces nrz5g - the game thread - and nrz5t - the translation thread); the host test is tests/nr_z5r_test.cpp (two real threads).
//
// nr_z5w.h (section 18) hands a run over at its first tail record under ONE verdict for the whole run - a verdict nobody can give
// yet: it would have to vouch, before the run's records are executed, for everything each of its draws does. This protocol asks
// less and is exact by construction:
//   writer   (the game thread, per engine draw record, in record order) As nr_z5w.h: the model of the bind cache starts EMPTY at a
//            run; a draw that reads a field the run has not set is a head draw (mark 0: the translation thread records it); the first
//            draw that reads only known fields opens the tail - a spare chunk for its packets, a second spare chunk (the SIDE) for
//            the values each tail draw was written from (nrz5_pv), a slot of the ring. A draw the thread has NO values for ends the
//            run (whatever that draw binds, the run cannot know). The run is published when it ends: one atomic store a run.
//   reader   (the translation thread) executes EVERY record as today - the state, the prologue of draw(), descriptors, pass,
//            views, viewport - and at the place where the lane's tail would write the draw's commands asks Reader::draw with the
//            values it just resolved. A tail draw is VALID iff
//              (1) the real bind cache holds, in every field this draw reads, what the run's model held there (the shadow: the
//                  run's at_splice fields, then every valid draw's values) - so the packets left out for it are left out today too;
//              (2) the values the game thread wrote its packets from are, field for field, the values resolved now.
//            Then its packets in the chunk are the packets the tail would write: nothing is written, the caller sets the real
//            cache to the draw's values (what the tail leaves). The chunk is HELD while draws are valid and handed over
//            (splice_spare, at the position of the first valid draw: nothing else was written since - see flush) up to the last
//            valid one when: a tail draw is not valid, a record of the run does not come (the prologue dropped the draw, another
//            record), the run is complete, or ANYTHING else is about to be written into the command buffer (the engine calls
//            flush() from the hook every vkCmd* wrapper runs first). After a cut the rest of the run is recorded as today.
//   verify   (DM_NR_Z5=2) nothing is held or handed over: the translation thread records every draw and, for each draw that would
//            have been valid, compares its own packets with the chunk's, packet for packet; a packet written between two tail
//            draws ends the run as flush() would.
// So nothing is assumed about how the game thread came to its values (nr_z5p.h: a prediction from the last time), about the
// prologue of draw() (it runs), or about state the game thread cannot see: a wrong or stale prediction costs the run, never a draw.
// What the translation thread saves per valid draw is only the command section (bind-cache compares and packets); the price is
// the comparison of the values. NR-ZERO.md section 22 says what that is worth and what the next step is.
#ifndef DM_NR_Z5R_H
#define DM_NR_Z5R_H
#include <atomic>
#include <stdint.h>
#include "nr_z5.h"
#include "../nb/nb_mt.h"

// The mutation switch of the host test (never defined in the engine build): NRZ5R_MUTATE = k - 1 the pipeline is not compared with
// the predicted one, 2 the dynamic offsets are not, 3 the vertex streams are not, 4 the draw arguments are not, 5 the reader does not
// ask whether the real cache holds the shadow, 6 a cut hands over the whole chunk, 7 the shadow is not advanced after a valid draw,
// 8 the reader takes the ring's next run whatever its id, 9 a covered record of another run is taken as the next draw of the run in
// hand, 10 a valid draw takes one packet of the chunk (not all up to its draw command), 11 a run without a valid draw is handed over,
// 12 the index buffer is not compared, 13 viewport / scissor are not, 14 the set / layout are not.
// (The writer has no mutation that must fail: the reader assumes nothing about it. A writer that writes ahead a draw reading a field
// its run never set, or keeps its run across a draw it had no values for, only loses runs - cache_holds compares every field a draw
// reads, known to the run or not, and the packets depend on the model's values alone.)
#ifndef NRZ5R_MUTATE
#define NRZ5R_MUTATE 0
#endif

namespace nrz5r {

enum { MARK_NONE = 0, MARK_SPLICE = 1, MARK_COVERED = 2, ID_MAX = 0x3fffffff };
inline uint32_t mark_word(unsigned mark, uint32_t id) { return id << 2 | mark; }   // the word a draw record carries (0 = MARK_NONE)
inline unsigned mark_of(uint32_t w) { return w & 3; }
inline uint32_t id_of(uint32_t w) { return w >> 2; }

// ---- the values a tail draw's packets were written from (one record per tail draw in the run's side chunk) ----
// Pv, then off[noff] (padded to 8 bytes), then {buffer, offset} of each used vertex binding in ascending order.
struct Pv { uint64_t pipe, layout, set, ib, ibo; uint8_t vp[NRZ5_VP_BYTES], sc[NRZ5_SC_BYTES];
            uint32_t it, count, instances, first, first_instance; int32_t vertex_offset; uint16_t used; uint8_t indexed, noff; uint32_t bytes; };
static_assert(sizeof(Pv) % 8 == 0, "nr_z5r.h: 8-aligned values record");
inline uint32_t pv_bytes(const nrz5_vals *v) { unsigned nvb = 0; for (uint32_t m = v->used; m; m &= m - 1) ++nvb; return uint32_t(sizeof(Pv) + nbmtq::al8(size_t(v->noff) * 4) + size_t(nvb) * 16); }
// 0 = no room (or more offsets than a record counts).
inline uint32_t pv_put(uint8_t *dst, size_t cap, const nrz5_vals *v) {
    if (v->noff > 255) return 0;
    const uint32_t n = pv_bytes(v); if (cap < n) return 0;
    Pv h; memset(&h, 0, sizeof h);
    h.pipe = v->pipe; h.layout = v->layout; h.set = v->set; memcpy(h.vp, v->vp, sizeof h.vp); memcpy(h.sc, v->sc, sizeof h.sc);
    h.used = v->used; h.indexed = v->indexed ? 1 : 0; h.noff = uint8_t(v->noff); h.bytes = n;
    if (v->indexed) { h.ib = v->ib; h.ibo = v->ibo; h.it = v->it; h.vertex_offset = v->vertex_offset; }
    h.count = v->count; h.instances = v->instances; h.first = v->first; h.first_instance = v->first_instance;
    memcpy(dst, &h, sizeof h); uint8_t *p = dst + sizeof h;
    memset(p, 0, nbmtq::al8(size_t(v->noff) * 4)); if (v->noff) memcpy(p, v->off, size_t(v->noff) * 4); p += nbmtq::al8(size_t(v->noff) * 4);
    for (uint32_t m = v->used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); memcpy(p, &v->vb[i], 8); memcpy(p + 8, &v->vbo[i], 8); p += 16; }
    return n;
}
// Every field nrz5_emit reads from the values, against the values resolved now. 0 = the same; else the first field that differs:
// 1 pipeline, 2 set / layout, 3 dynamic offsets, 4 viewport, 5 scissor, 6 vertex streams, 7 index buffer, 8 draw arguments, 9 a
// record that is not one (never from a whole side chunk).
inline int pv_diff(const uint8_t *rec, uint32_t avail, const nrz5_vals *r) {
    Pv h; if (avail < sizeof h) return 9; memcpy(&h, rec, sizeof h);
    if (h.bytes < sizeof h || h.bytes > avail) return 9;
    if (NRZ5R_MUTATE != 1 && h.pipe != r->pipe) return 1;
    if (NRZ5R_MUTATE != 14 && (h.layout != r->layout || h.set != r->set)) return 2;
    if (h.noff != r->noff || h.used != r->used || uint32_t(h.indexed) != uint32_t(r->indexed ? 1 : 0)) return h.noff != r->noff ? 3 : h.used != r->used ? 6 : 8;
    if (h.bytes != pv_bytes(r)) return 9;
    const uint8_t *p = rec + sizeof h;
    if (NRZ5R_MUTATE != 2 && h.noff && memcmp(p, r->off, size_t(h.noff) * 4)) return 3;
    p += nbmtq::al8(size_t(h.noff) * 4);
    if (NRZ5R_MUTATE != 13) { if (memcmp(h.vp, r->vp, sizeof h.vp)) return 4; if (memcmp(h.sc, r->sc, sizeof h.sc)) return 5; }
    for (uint32_t m = h.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); uint64_t b, o; memcpy(&b, p, 8); memcpy(&o, p + 8, 8); p += 16;
        if (NRZ5R_MUTATE != 3 && (b != r->vb[i] || o != r->vbo[i])) return 6; }
    if (h.indexed && NRZ5R_MUTATE != 12 && (h.ib != r->ib || h.ibo != r->ibo || h.it != r->it)) return 7;
    if (NRZ5R_MUTATE != 4) {
        if (h.count != r->count || h.instances != r->instances || h.first != r->first || h.first_instance != r->first_instance) return 8;
        if (h.indexed && h.vertex_offset != r->vertex_offset) return 8;
    }
    return 0;
}

// The real bind cache (as nr_z5.h's words, every field known) holds, in every field a draw with the values v reads, what the shadow
// says. (A tail draw reads only fields the shadow knows.)
inline bool cache_holds(const nrz5_cache *sh, const nrz5_cache *real, const nrz5_vals *v) {
    if (sh->pipe != real->pipe) return false;
    if (sh->noff != real->noff) return false;
    if (sh->noff != 0xffffffffu && (sh->layout != real->layout || sh->set != real->set || (sh->noff && memcmp(sh->off, real->off, size_t(sh->noff) * 4)))) return false;
    if (!real->has_vp || memcmp(sh->vp, real->vp, NRZ5_VP_BYTES) || !real->has_sc || memcmp(sh->sc, real->sc, NRZ5_SC_BYTES)) return false;
    for (uint32_t m = v->used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); if (sh->vb[i] != real->vb[i] || sh->vbo[i] != real->vbo[i]) return false; }
    if (v->indexed && (sh->ib != real->ib || sh->ibo != real->ibo || sh->it != real->it)) return false;
    return true;
}

struct Run {
    nbmtq::Chunk *chunk = nullptr, *side = nullptr;   // the tail's packets; the values each tail draw was written from
    uint32_t id = 0, tails = 0;                       // tails: the records the chunk covers (the MARK_SPLICE one and the MARK_COVERED ones after it)
    nrz5_cache at_splice;                             // what the run knew of the bind cache before its first tail draw
};
// Single producer (the writer thread), single consumer (the translation thread). A run is in flight from push to pop.
struct Ring {
    enum { N = 1024 };
    Run slot[N];
    std::atomic<uint32_t> w{0}, r{0};
    bool room() const { return w.load(std::memory_order_relaxed) - r.load(std::memory_order_acquire) < N; }   // writer
    void push(const Run &x) { const uint32_t i = w.load(std::memory_order_relaxed); slot[i % N] = x; w.store(i + 1, std::memory_order_release); }   // writer, after room()
    Run *front() { const uint32_t i = r.load(std::memory_order_relaxed); return i == w.load(std::memory_order_acquire) ? nullptr : &slot[i % N]; }   // reader
    void pop() { r.store(r.load(std::memory_order_relaxed) + 1, std::memory_order_release); }                                                       // reader
};

struct WStats { uint64_t draws = 0, no_values = 0, heads = 0, tails = 0, runs = 0, no_chunk = 0, ring_full = 0, chunk_full = 0, short_runs = 0, short_tails = 0, tail_bytes = 0, side_bytes = 0; };
struct Writer {
    nbmtq::Queue *q = nullptr; Ring *ring = nullptr;
    nrz5_cache model; bool in_run = false; Run cur; uint32_t next_id = 1; WStats st;
    uint32_t min_tails = 1;              // a tail shorter than this is not worth a hand-over: its run is published WITHOUT chunks (the reader records its records as today)
    // [NR-ZERO Z8, the walk on its own thread] 0: no limit. A tail that reached this many draws is published at once and the draw in hand
    // opens the next run on the model as it stands; the limit doubles at every split and ends above 2048. So a reader that follows close
    // behind the writer finds the first runs of a recording published when it reaches them.
    uint32_t max_tails = 0; uint64_t splits = 0;
    uint32_t last_bytes = 0, last_cmds = 0;   // [NR-ZERO Z8 values] the packets the last draw() wrote into the run's chunk (0: a head draw)
    // (The reader checks, per draw, that the real bind cache holds the run's model in every field the draw reads: a run that leans on the
    // one before it is taken only if that one's draws - taken, or recorded by the reader itself - left the same values.)
    void split() { end_run(); in_run = true; cur = Run{}; ++splits; }
    void head(const nrz5_vals &v) { nrz5_after(&model, &v); ++st.heads; }
    void restart() { end_run(); nrz5_cache_unknown(&model); in_run = true; cur = Run{}; }
    // An engine draw record is being written and the thread has values for it (v). emit(chunk, model, vals) writes the draw's packets
    // into the chunk over the model (nrz5_emit with a sink on the chunk): the number of commands, 0 = the chunk is full. Returns the
    // record's mark word.
    template <class Emit> uint32_t draw(const nrz5_vals &v, Emit &&emit) {
        ++st.draws; last_bytes = last_cmds = 0;
        if (!in_run) { nrz5_cache_unknown(&model); in_run = true; cur = Run{}; }
        if (!cur.chunk) {
            if (nrz5_reads_unknown(&model, &v)) { head(v); return 0; }
            // the first tail draw: a slot of the ring and two spare chunks - or it stays the translation thread's, like a head draw
            if (!ring->room()) { ++st.ring_full; head(v); return 0; }
            nbmtq::Chunk *c = q->take_spare(), *s = c ? q->take_spare() : nullptr;
            if (!s) { if (c) q->give_back_spare(c); ++st.no_chunk; head(v); return 0; }
            cur.chunk = c; cur.side = s; cur.id = next_id; next_id = next_id == ID_MAX ? 1 : next_id + 1; cur.tails = 0; cur.at_splice = model;
        } else if (nrz5_reads_unknown(&model, &v)) { restart(); head(v); return 0; }   // a new unknown field after the tail began: this draw is the head of the next run
        if (max_tails && cur.tails >= max_tails) {   // the limit: publish; this draw opens the next run (or stays a head draw when there is no room for one)
            split(); max_tails = max_tails >= 2048 ? 0 : max_tails * 2;
            if (!ring->room()) { ++st.ring_full; head(v); return 0; }
            nbmtq::Chunk *c = q->take_spare(), *s = c ? q->take_spare() : nullptr;
            if (!s) { if (c) q->give_back_spare(c); ++st.no_chunk; head(v); return 0; }
            cur.chunk = c; cur.side = s; cur.id = next_id; next_id = next_id == ID_MAX ? 1 : next_id + 1; cur.tails = 0; cur.at_splice = model;
        }
        const nrz5_cache saved = model; const uint32_t u = cur.chunk->used, cm = cur.chunk->cmds;
        uint32_t pvn = 0;
        if (!emit(cur.chunk, &model, &v) || !(pvn = pv_put(cur.side->data() + cur.side->used, cur.side->cap - cur.side->used, &v))) {   // a chunk is full: the run ends before this draw
            model = saved; cur.chunk->used = u; cur.chunk->cmds = cm; ++st.chunk_full;
            if (!cur.tails) { q->give_back_spare(cur.chunk); q->give_back_spare(cur.side); cur.chunk = cur.side = nullptr; head(v); return 0; }   // (not even one draw fits)
            restart(); head(v); return 0;
        }
        cur.side->used += pvn; ++st.tails; st.tail_bytes += cur.chunk->used - u; st.side_bytes += pvn; last_bytes = cur.chunk->used - u; last_cmds = cur.chunk->cmds - cm;
        return mark_word(cur.tails++ ? MARK_COVERED : MARK_SPLICE, cur.id);
    }
    // An engine draw record the thread has no values for (its mark is 0): the run ends - what that draw leaves bound is not known.
    void no_values() { ++st.draws; ++st.no_values; end_run(); }
    // The end of the recording, another command buffer, anything after which the next draw must not lean on this run.
    void end_run() {
        if (in_run && cur.chunk) {
            if (cur.tails < min_tails) { q->give_back_spare(cur.chunk); q->give_back_spare(cur.side); cur.chunk = cur.side = nullptr; ++st.short_runs; st.short_tails += cur.tails; }   // (its records carry the id: the run is published, empty)
            ring->push(cur); ++st.runs;
            cur.chunk = cur.side = nullptr;
        }
        in_run = false; cur = Run{};
    }
};

enum { CUT_CACHE = 0, CUT_VALUES, CUT_CHUNK, CUT_RECORD, CUT_WRITE, CUT_N };   // why a run was cut: the real cache does not hold the shadow, the values differ, the chunk / side ends early, a record of the run did not come (or another draw record did), something else was written
struct RStats { uint64_t runs = 0, started = 0, complete = 0, handed = 0, valid = 0, cut[CUT_N] = {}, diff[10] = {}, first_no = 0, gone = 0, unreached = 0, declined_writer = 0;
                uint64_t v_cmp = 0, v_same = 0, v_bad = 0, v_skipped = 0, known = 0, known_cut = 0, k_cmp = 0, k_bad = 0; };
struct Reader {
    enum { RECORD = 0, VALID = 1 };
    nbmtq::Queue *q = nullptr; Ring *ring = nullptr; RStats st;
    bool holding = false;                 // a run is being followed (apply: its chunk is held; verify: compared)
    Run run; uint32_t j = 0, at = 0, cmds = 0, side_at = 0, from = 0;   // valid draws so far; the chunk's bytes / packets they take; the side's; where the last valid draw's packets begin
    nrz5_cache shadow;
    bool v_this = false; uint64_t v_sig = 0;   // verify: the draw in hand would be valid; the stream position after the last compared draw
    // [NR-ZERO Z8 values, NR-ZERO.md section 31] What the caller knows of the tail draw in hand WITHOUT the run's chunks: d - how the
    // values the writer wrote the draw's packets from differ from the values resolved now (pv_diff's codes, 0 = the same), named
    // from the caller's own memory (the template the writer read is the caller's own, and so are the words of State the writer
    // folded); bytes / n - the draw's packets in the chunk, as the writer counted them. draw2() with it reads neither the side
    // chunk nor the chunk's packets (both are memory another thread has just written). A run is followed one way from its first
    // tail draw to its last (known_run); `chk` (verify of this part): step() compares what it finds in the chunks with it.
    struct Known { int d; uint32_t bytes, n; };
    bool known_run = false; const Known *chk = nullptr;
    template <class Holds> bool step_known(const nrz5_vals &rv, const Known &k, Holds &&holds, int *why) {
        nbmtq::Chunk *c = run.chunk;
        if (k.d) { ++st.diff[k.d < 10 ? k.d : 9]; *why = CUT_VALUES; return false; }
        if (NRZ5R_MUTATE != 5 && !holds(shadow, rv)) { *why = CUT_CACHE; return false; }
        if (!k.bytes || !k.n || k.bytes > c->used - at) { *why = CUT_CHUNK; return false; }
        from = at; at += k.bytes; cmds += k.n; ++j; ++st.valid; ++st.known;
        if (NRZ5R_MUTATE != 7) nrz5_after(&shadow, &rv);
        return true;
    }

    static bool id_before(uint32_t a, uint32_t b) { return a != b && ((b - a) & uint32_t(ID_MAX)) < uint32_t(ID_MAX) / 2; }
    // The ring's run of a MARK_SPLICE record (as nr_z5w.h: ids are handed out in record order; an older run at the front belongs to a
    // recording that was never executed and is given back; a newer one: this record's run is no longer there).
    bool next_run(uint32_t id, Run &out) {
        for (;;) {
            Run *r = ring->front();
            if (!r) { ++st.gone; return false; }
            if (NRZ5R_MUTATE == 8 || r->id == id) { out = *r; ring->pop(); ++st.runs; return true; }
            if (!id_before(r->id, id)) { ++st.gone; return false; }
            q->give_back_spare(r->chunk); q->give_back_spare(r->side); ring->pop(); ++st.unreached;
        }
    }
    // The next tail draw of the run in hand against the values resolved now. holds(shadow, rv): the real bind cache holds, in every
    // field a draw with these values reads, what the shadow says (the shadow's unknown fields are never read by a tail draw).
    template <class Holds, class IsDraw> bool step(const nrz5_vals &rv, Holds &&holds, IsDraw &&is_draw, int *why) {
        nbmtq::Chunk *c = run.chunk, *s = run.side;
        if (side_at >= s->used) { *why = CUT_CHUNK; return false; }
        const int d = pv_diff(s->data() + side_at, s->used - side_at, &rv);
        if (chk) { ++st.k_cmp; if (chk->d != d && d != 9) ++st.k_bad; }
        if (d) { ++st.diff[d]; *why = d == 9 ? CUT_CHUNK : CUT_VALUES; return false; }
        if (NRZ5R_MUTATE != 5 && !holds(shadow, rv)) { *why = CUT_CACHE; return false; }
        uint32_t end = at, n = 0; bool found = false;
        while (end < c->used) { const nbmtq::Pkt *p = reinterpret_cast<const nbmtq::Pkt *>(c->data() + end); if (p->size < sizeof(nbmtq::Pkt) || p->size > c->used - end) break; end += p->size; ++n; if (is_draw(p->op) || NRZ5R_MUTATE == 10) { found = true; break; } }
        if (!found) { *why = CUT_CHUNK; return false; }
        if (chk && (chk->bytes != end - at || chk->n != n)) ++st.k_bad;
        Pv h; memcpy(&h, s->data() + side_at, sizeof h);
        from = at; at = end; cmds += n; side_at += h.bytes; ++j; ++st.valid;
        if (NRZ5R_MUTATE != 7) nrz5_after(&shadow, &rv);
        return true;
    }
    bool begin_run(uint32_t id) {
        if (!next_run(id, run)) return false;
        if (!run.chunk) { ++st.declined_writer; return false; }
        shadow = run.at_splice; j = 0; at = 0; cmds = 0; side_at = 0; holding = true; known_run = false; ++st.started;
        return true;
    }
    // ---- apply ----
    // The validated draws' packets go into the stream here; the run is over. The engine calls it (why = CUT_WRITE) before anything else
    // is written into the command buffer and at its end, and with CUT_RECORD when a record of the run did not reach its command section.
    void flush(int why) {
        if (!holding) return;
        holding = false;
        if (j < run.tails) ++st.cut[why]; else ++st.complete;
        if (!j && NRZ5R_MUTATE != 11) { ++st.first_no; q->give_back_spare(run.chunk); }
        else { if (NRZ5R_MUTATE != 6 && NRZ5R_MUTATE != 11) { run.chunk->used = at; run.chunk->cmds = cmds; } q->splice_spare(run.chunk); ++st.handed; }
        q->give_back_spare(run.side);
    }
    // An engine draw record with mark word mw reached the command section of the lane's tail with the values rv.
    // VALID: nothing is to be recorded - the caller writes the draw's values into the real bind cache (what the tail leaves there).
    // RECORD: the caller records the draw as today (whatever was held is in the stream already).
    template <class Holds, class IsDraw> int draw(uint32_t mw, const nrz5_vals &rv, Holds &&holds, IsDraw &&is_draw) {
        const unsigned m = mark_of(mw); const uint32_t id = id_of(mw); int why = CUT_RECORD;
        if (holding) {
            if (m == MARK_COVERED && (id == run.id || NRZ5R_MUTATE == 9) && step(rv, holds, is_draw, &why)) { if (j == run.tails) flush(CUT_WRITE); return VALID; }
            flush(why);
        }
        if (m == MARK_SPLICE && begin_run(id)) {
            if (step(rv, holds, is_draw, &why)) { if (j == run.tails) flush(CUT_WRITE); return VALID; }
            flush(why);
        }
        return RECORD;
    }
    // draw() when the caller may know the draw's verdict itself (k: null - it does not). A run whose first tail draw came with k is
    // followed by step_known alone: a later draw of it without k ends it there.
    template <class Holds, class IsDraw> int draw2(uint32_t mw, const nrz5_vals &rv, const Known *k, Holds &&holds, IsDraw &&is_draw) {
        const unsigned m = mark_of(mw); const uint32_t id = id_of(mw); int why = CUT_RECORD;
        auto one = [&] { if (!known_run) return step(rv, holds, is_draw, &why); if (!k) { ++st.known_cut; return false; } return step_known(rv, *k, holds, &why); };
        if (holding) {
            if (m == MARK_COVERED && id == run.id && one()) { if (j == run.tails) flush(CUT_WRITE); return VALID; }
            flush(why);
        }
        if (m == MARK_SPLICE && begin_run(id)) {
            known_run = k != nullptr;
            if (one()) { if (j == run.tails) flush(CUT_WRITE); return VALID; }
            flush(why);
        }
        return RECORD;
    }
    // A record of a recording that will never be executed (optional: a run nobody abandons is given back at the next MARK_SPLICE).
    void abandon(uint32_t mw) { if (mark_of(mw) != MARK_SPLICE) return; Run x; if (next_run(id_of(mw), x)) { --st.runs; ++st.unreached; q->give_back_spare(x.chunk); q->give_back_spare(x.side); } }
    // Every run the ring holds goes back (shutdown; the records that carry their ids are then recorded as today). A held run first.
    void drop() { if (holding) { holding = false; q->give_back_spare(run.chunk); q->give_back_spare(run.side); }
                  while (Run *r = ring->front()) { q->give_back_spare(r->chunk); q->give_back_spare(r->side); ring->pop(); ++st.unreached; } }

    // ---- verify: nothing is held or handed over ----
    void v_end(int why, bool counted) { if (!holding) return; holding = false; if (counted) { if (j < run.tails) ++st.cut[why]; else ++st.complete; if (!j) ++st.first_no; else ++st.handed; }
                                         q->give_back_spare(run.chunk); q->give_back_spare(run.side); }
    // Before the translation thread records the draw of a record itself (at the command section, the values in hand). sig: a number
    // that moves whenever anything is written into the command buffer (the queue's packet count + its direct calls). Returns whether
    // DM_NR_Z5=1 would have taken this draw.
    template <class Holds, class IsDraw> bool verify_begin(uint32_t mw, const nrz5_vals &rv, Holds &&holds, IsDraw &&is_draw, uint64_t sig) {
        const unsigned m = mark_of(mw); const uint32_t id = id_of(mw); int why = CUT_RECORD; v_this = false;
        if (holding) {
            if (sig != v_sig) v_end(CUT_WRITE, true);
            else if (m == MARK_COVERED && id == run.id && step(rv, holds, is_draw, &why)) v_this = true;
            else v_end(why, true);
        }
        if (!v_this && m == MARK_SPLICE && begin_run(id)) { if (step(rv, holds, is_draw, &why)) v_this = true; else v_end(why, true); }
        return v_this;
    }
    // After it recorded that draw. mine / n: its own packets of the draw (null: not readable - a hand-over happened inside the draw);
    // same(a, na, b, nb): the stream comparison (xbg_nb_mt.inc same_stream).
    template <class Same> void verify_end(const uint8_t *mine, uint32_t n, Same &&same, uint64_t sig_after) {
        if (!v_this) return; v_this = false;
        nbmtq::Chunk *c = run.chunk;
        if (!mine) ++st.v_skipped;
        else { ++st.v_cmp; if (same(mine, n, c->data() + from, at - from)) ++st.v_same; else ++st.v_bad; }
        v_sig = sig_after;
        if (j == run.tails) { if (at != c->used) ++st.v_bad; v_end(CUT_WRITE, true); }   // the chunk ends where the tail does
    }
    // A record of the run did not reach its command section (verify's twin of flush(CUT_RECORD)).
    void verify_miss() { v_this = false; v_end(CUT_RECORD, true); }
};

} // namespace nrz5r
#endif
