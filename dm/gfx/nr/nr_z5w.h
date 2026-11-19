// [DOS2] NR-ZERO Z5, the second writer (DM_NR_Z5, docs/dos2/nr/NR-ZERO.md section 18): how a thread that is NOT the translation thread
// (the game thread, while it writes the draw records) hands the translation thread the packets of a run's tail, and how the
// translation thread takes or declines them. C++, no Vulkan / engine type: the run assembler is nr_z5.h, the queue nb_mt.h; the host
// test is dm/gfx/vulkan/engine/tests/nr_z5w_test.cpp (two real threads).
//
// NOT wired into the engine: the game thread has no values to feed it yet (the pipeline, set, ring offsets, views and viewport of a
// draw are resolved by the translation thread; NR-ZERO.md section 18.3 lists what is missing). This file is the packet half of that
// writer, exact by itself and tested on the PC: when the values exist, the engine calls Writer::draw where it writes an OP_X_NR_DRAW
// and Reader::draw where nrdraw::lane_tail reaches its command section.
//
// The protocol.
//   writer   Per recording, in record order. A run = consecutive engine draw records (Writer::end_run at any other record). The model
//            of the bind cache starts EMPTY (nr_z5.h): a draw that reads a field the run has not set is a head draw - its record is
//            marked MARK_NONE and the translation thread records it as today. The first draw that reads only known fields opens the
//            tail: a spare chunk (nbmtq::Queue::take_spare: one lock a chunk) and a slot of the ring; its record is marked
//            MARK_SPLICE with the run's id, the following tail draws MARK_COVERED with the same id. The run is published (Ring::push:
//            one atomic store a run, none a packet or a draw) when it ends - before the recording is submitted, so the translation
//            thread never meets a mark whose run is not in the ring.
//   reader   The translation thread, at each engine draw record, in record order (Reader::draw):
//            MARK_NONE     -> records the draw itself.
//            MARK_SPLICE   -> the ring's run of that id (older runs - of recordings never executed - are given back on the way; a run
//                             that is no longer there: the record is recorded as today). The run is TAKEN only if (1) the real bind cache
//                             holds every field the run knew before its first tail draw (Run::at_splice against the cache, field by
//                             field: nothing is assumed about how the head draws were recorded or what else touched the cache), and
//                             (2) the caller's own verdict `ok(run)` - everything else the tail's draws would do on the translation
//                             thread besides their commands (the stamps of NR-ZERO.md section 18.3; it may look at the run's
//                             records, they are the next Run::tails ones). Taken: the chunk is spliced at this position
//                             (splice_spare), the caller copies Run::after's known fields into the real cache, and the
//                             MARK_COVERED records of the id are skipped. Declined: the chunk goes back (give_back_spare) and every
//                             record of the run is recorded as today - the fallback is the path of today, draw by draw.
//            MARK_COVERED  -> skipped if its run was taken, else recorded.
//   verify   (DM_NR_Z5=2's shape with a second writer) Reader::verify_*: nothing is taken; the translation thread records every
//            draw and compares its own packets of each tail draw with the run's chunk, packet for packet, in order; the chunk must
//            end exactly where the tail does.
// Order with passes / barriers: unchanged by construction - a run never spans a record that is not an engine draw, and its chunk is
// handed over by the queue's one hand-over at the position of its first tail draw (nb_mt.h splice).
#ifndef DM_NR_Z5W_H
#define DM_NR_Z5W_H
#include <atomic>
#include <stdint.h>
#include "nr_z5.h"
#include "../nb/nb_mt.h"

// The mutation switch of the host test (never defined in the engine build): NRZ5W_MUTATE = k - 1 the reader does not compare the real
// bind cache with what the run knew before its first tail draw, 2 the caller's verdict is not asked, 3 the covered records of a
// declined run are skipped, 4 the writer keeps writing ahead when a draw reads a field the run has not set, 5 the reader takes
// the ring's next run whatever its id, 6 the run does not remember the cache after its tail (the fields to copy back are the
// ones before it).
#ifndef NRZ5W_MUTATE
#define NRZ5W_MUTATE 0
#endif

namespace nrz5w {

enum { MARK_NONE = 0, MARK_SPLICE = 1, MARK_COVERED = 2, ID_MAX = 0x3fffffff };
inline uint32_t mark_word(unsigned mark, uint32_t id) { return id << 2 | mark; }   // the word a draw record carries (0 = MARK_NONE)
inline unsigned mark_of(uint32_t w) { return w & 3; }
inline uint32_t id_of(uint32_t w) { return w >> 2; }

struct Run {
    nbmtq::Chunk *chunk = nullptr;
    uint32_t id = 0, tails = 0;          // tails: the records the chunk covers (the MARK_SPLICE one and the MARK_COVERED ones after it)
    nrz5_cache at_splice, after;         // what the run knows of the bind cache before its first tail draw / after its last
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

struct WStats { uint64_t draws = 0, heads = 0, tails = 0, runs = 0, no_chunk = 0, ring_full = 0, chunk_full = 0, short_runs = 0, short_tails = 0; };
struct Writer {
    nbmtq::Queue *q = nullptr; Ring *ring = nullptr;
    nrz5_cache model; bool in_run = false; Run cur; uint32_t next_id = 1; WStats st;
    uint32_t min_tails = 1;              // a tail shorter than this is not worth two hand-overs: its run is published WITHOUT a chunk (the reader records its records as today)
    void head(const nrz5_vals &v) { nrz5_after(&model, &v); ++st.heads; }
    // An engine draw record is being written; v: the values its command section will hold. emit(chunk, model, vals) writes the draw's
    // packets into the chunk over the model (nrz5_emit with a sink on the chunk): the number of commands, 0 = the chunk is full.
    // Returns the record's mark word.
    template <class Emit> uint32_t draw(const nrz5_vals &v, Emit &&emit) {
        ++st.draws;
        if (!in_run) { nrz5_cache_unknown(&model); in_run = true; cur = Run{}; }
        if (!cur.chunk) {
            if (nrz5_reads_unknown(&model, &v)) { head(v); return 0; }
            // the first tail draw: a slot of the ring and a spare chunk - or it stays the translation thread's, like a head draw
            if (!ring->room()) { ++st.ring_full; head(v); return 0; }
            nbmtq::Chunk *c = q->take_spare();
            if (!c) { ++st.no_chunk; head(v); return 0; }
            cur.chunk = c; cur.id = next_id; next_id = next_id == ID_MAX ? 1 : next_id + 1; cur.tails = 0; cur.at_splice = model;
        } else if (NRZ5W_MUTATE != 4 && nrz5_reads_unknown(&model, &v)) { end_run(); nrz5_cache_unknown(&model); in_run = true; cur = Run{}; head(v); return 0; }   // a new unknown field after the tail began: this draw is the head of the next run
        const nrz5_cache saved = model; const uint32_t u = cur.chunk->used, cm = cur.chunk->cmds;
        if (!emit(cur.chunk, &model, &v)) {   // the chunk is full: the run ends before this draw
            model = saved; cur.chunk->used = u; cur.chunk->cmds = cm; ++st.chunk_full;
            if (!cur.tails) { q->give_back_spare(cur.chunk); cur.chunk = nullptr; head(v); return 0; }   // (not even one draw fits)
            end_run(); nrz5_cache_unknown(&model); in_run = true; cur = Run{}; head(v); return 0;
        }
        ++st.tails;
        return mark_word(cur.tails++ ? MARK_COVERED : MARK_SPLICE, cur.id);
    }
    // Any record that is not an engine draw of the lane; the end of the recording.
    void end_run() {
        if (in_run && cur.chunk) {
            if (cur.tails < min_tails) { q->give_back_spare(cur.chunk); cur.chunk = nullptr; ++st.short_runs; st.short_tails += cur.tails; }   // (its records carry the id: the run is published, empty)
            cur.after = NRZ5W_MUTATE == 6 ? cur.at_splice : model; ring->push(cur); ++st.runs;
            cur.chunk = nullptr;
        }
        in_run = false; cur = Run{};
    }
};

struct RStats { uint64_t runs = 0, taken = 0, declined_cache = 0, declined_stamp = 0, declined_writer = 0, covered = 0, gone = 0, unreached = 0;
                uint64_t v_runs = 0, v_cmp = 0, v_same = 0, v_bad = 0, v_skipped = 0, v_not_comparable = 0; };
struct Reader {
    enum { RECORD = 0, SPLICED = 1, SKIP = 2 };
    nbmtq::Queue *q = nullptr; Ring *ring = nullptr;
    uint32_t cur_id = 0; bool taken = false; RStats st;
    // verify: the run being compared
    Run vrun; bool v_on = false, v_ok = false; uint32_t v_at = 0, v_left = 0;
    // The ring's run of a MARK_SPLICE record (out). Ids are handed out in record order, so the ring's front is this record's run, an
    // OLDER one (a run of a recording that was never executed: given back, the next one is looked at) or a NEWER one (this record's
    // run was given back before - drop()): false, the record is recorded as today and the ring is left alone.
    static bool id_before(uint32_t a, uint32_t b) { return a != b && ((b - a) & uint32_t(ID_MAX)) < uint32_t(ID_MAX) / 2; }
    bool next_run(uint32_t id, Run &out) {
        for (;;) {
            Run *r = ring->front();
            if (!r) { ++st.gone; return false; }
            if (NRZ5W_MUTATE == 5 || r->id == id) { out = *r; ring->pop(); ++st.runs; return true; }
            if (!id_before(r->id, id)) { ++st.gone; return false; }
            q->give_back_spare(r->chunk); ring->pop(); ++st.unreached;
        }
    }
    // A record of a recording that will never be executed (optional: a run nobody abandons is given back at the next MARK_SPLICE).
    void abandon(uint32_t mw) { if (mark_of(mw) != MARK_SPLICE) return; Run run; if (next_run(id_of(mw), run)) { --st.runs; ++st.unreached; q->give_back_spare(run.chunk); } }
    // An engine draw record with mark word mw reaches its command section. real(): the bind cache of now in nr_z5.h's words;
    // ok(run): the caller's verdict on the rest (see the header). SPLICED: *after = the fields to copy into the real cache.
    template <class Real, class Ok> int draw(uint32_t mw, Real &&real, Ok &&ok, nrz5_cache *after) {
        const unsigned m = mark_of(mw); const uint32_t id = id_of(mw);
        if (m == MARK_NONE) return RECORD;
        if (m == MARK_SPLICE) {
            Run run; cur_id = id; taken = false;
            if (!next_run(id, run)) return RECORD;
            if (!run.chunk) { ++st.declined_writer; return RECORD; }   // the writer published the run without a chunk (too short)
            const nrz5_cache rc = real();
            if (NRZ5W_MUTATE != 1 && nrz5_cache_diff(&run.at_splice, &rc)) { ++st.declined_cache; q->give_back_spare(run.chunk); return RECORD; }
            if (NRZ5W_MUTATE != 2 && !ok(run)) { ++st.declined_stamp; q->give_back_spare(run.chunk); return RECORD; }
            q->splice_spare(run.chunk); *after = run.after; taken = true; ++st.taken;
            return SPLICED;
        }
        if (id == cur_id && (taken || NRZ5W_MUTATE == 3)) { ++st.covered; return SKIP; }
        return RECORD;
    }
    // Every run the ring holds goes back (shutdown; the records that carry their ids are then recorded as today).
    void drop() { while (Run *r = ring->front()) { q->give_back_spare(r->chunk); ring->pop(); ++st.unreached; } taken = false; if (v_on) { q->give_back_spare(vrun.chunk); v_on = false; } }

    // ---- verify: nothing is taken ----
    // Before the translation thread records the draw of a marked record itself. comparable: the run could have been taken here (the
    // cache held its fields and the caller's verdict is yes) - only then must the packets agree.
    template <class Real, class Ok> void verify_begin(uint32_t mw, Real &&real, Ok &&ok) {
        const unsigned m = mark_of(mw); const uint32_t id = id_of(mw);
        if (m == MARK_NONE) return;
        if (m == MARK_SPLICE) {
            if (v_on) { q->give_back_spare(vrun.chunk); v_on = false; }
            if (!next_run(id, vrun)) return;
            if (!vrun.chunk) { ++st.declined_writer; return; }
            cur_id = id; v_on = true; v_at = 0; v_left = vrun.tails; ++st.v_runs;
            const nrz5_cache rc = real();
            v_ok = !nrz5_cache_diff(&vrun.at_splice, &rc) && ok(vrun);
            if (!v_ok) ++st.v_not_comparable;
        }
    }
    // After it recorded that draw. mine / n: its own packets of the draw (null: not readable - a hand-over happened inside the draw);
    // same(a, na, b, nb): the stream comparison (xbg_nb_mt.inc same_stream); is_draw(op): the packet is the draw command itself.
    template <class Same, class IsDraw> void verify_end(uint32_t mw, const uint8_t *mine, uint32_t n, Same &&same, IsDraw &&is_draw) {
        if (!v_on || mark_of(mw) == MARK_NONE || id_of(mw) != cur_id) return;
        nbmtq::Chunk *c = vrun.chunk;
        // the run's packets of this draw: up to and with its draw command
        uint32_t end = v_at; bool found = false;
        while (end < c->used) { const nbmtq::Pkt *p = reinterpret_cast<const nbmtq::Pkt *>(c->data() + end); if (p->size < sizeof(nbmtq::Pkt) || p->size > c->used - end) break; end += p->size; if (is_draw(p->op)) { found = true; break; } }
        if (v_ok) {
            if (!found) { ++st.v_bad; }
            else if (!mine) ++st.v_skipped;
            else { ++st.v_cmp; if (same(mine, n, c->data() + v_at, end - v_at)) ++st.v_same; else ++st.v_bad; }
        }
        v_at = end;
        if (!--v_left) { if (v_ok && v_at != c->used) ++st.v_bad; q->give_back_spare(vrun.chunk); v_on = false; }   // the chunk ends where the tail does
    }
};

} // namespace nrz5w
#endif
