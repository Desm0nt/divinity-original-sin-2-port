// [DOS2] NB-20b DM_NB_MT: the ordered command-packet queue between the translate thread and one recording worker
// (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-20b findings"). No Vulkan and no engine here: the engine side is
// dm/gfx/vulkan/engine/xbg_nb_mt.inc (one packet per vkCmd*), the host test is dm/gfx/vulkan/engine/tests/nb_mt_test.cpp.
//
// Model. One producer (the translate thread, under the engine lock) writes packets into a chunk; a full chunk is handed over
// under one mutex (never per packet: the per-packet atomics of a ring cost as much as the vkCmd itself, KB PERF-E18). Chunks
// are replayed strictly in hand-over order into the one command buffer of the submission, by one thread at a time (`busy`):
// the worker, or the producer itself when it has to wait anyway ("help": at the submit, on a full queue). So the command
// buffer gets the same calls in the same order as without the queue, and it (and its command pool) is used by one thread at
// a time - the producer never touches it while chunks are queued or being replayed.
//   mode 1  no thread: a chunk is replayed by the producer at hand-over (the cost of the packets alone)
//   mode 2  worker + check: chunk hash before / after the replay against the producer's, hand-over order, packet walk
//   mode 3  worker
// Waiting is a condition variable (no spinning): the worker sleeps while the queue is empty, the producer sleeps only while
// the worker is inside a chunk and there is nothing the producer could replay itself.
#ifndef DM_NB_MT_H
#define DM_NB_MT_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace nbmtq {

enum { NBMT_SY_SUBMIT = 0, NBMT_SY_PNEXT, NBMT_SY_ALLOC, NBMT_SY_CALLER, NBMT_SY_STOP, NBMT_SY_KINDS };   // why the producer drained the queue
enum { NBMT_FB_POOL = 0, NBMT_FB_AB, NBMT_FB_THREAD, NBMT_FB_NESTED, NBMT_FB_KINDS };                       // why a submission was recorded directly
enum { NBMT_BAD_HASH_PRE = 0, NBMT_BAD_HASH_POST, NBMT_BAD_ORDER, NBMT_BAD_WALK, NBMT_BAD_KINDS };         // mode 2 findings

struct Pkt {                       // header of one deferred call; the call's arguments and array copies follow
    void (*run)(void *cb, const Pkt *p);
    uint32_t size;                 // bytes to the next packet (8-aligned)
    uint16_t op, pad;
};
struct Chunk {                     // packets of one hand-over; data follows the header
    Chunk *next;
    void *cb;
    uint64_t seq, hash;
    uint32_t cap, used, cmds, big;
    uint8_t *data() { return reinterpret_cast<uint8_t *>(this + 1); }
};
static_assert(sizeof(Pkt) % 8 == 0 && sizeof(Chunk) % 8 == 0, "nb_mt.h: 8-aligned headers");

inline uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
inline size_t al8(size_t n) { return (n + 7) & ~size_t(7); }
inline uint64_t hash_bytes(const uint8_t *p, size_t n) {   // n is a multiple of 8 (chunk contents)
    uint64_t h = 0x9E3779B97F4A7C15ull, w;
    for (size_t i = 0; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 0xBF58476D1CE4E5B9ull; h ^= h >> 29; }
    return h ^ n;
}

struct Stats {                     // producer side (translate thread only)
    uint64_t subs = 0, direct[NBMT_FB_KINDS] = {}, cmds = 0, chunks = 0, bytes = 0, big = 0, mallocs = 0, alloc_fail = 0;
    uint64_t sync[NBMT_SY_KINDS] = {}, waits = 0, wait_ns = 0, wait_max_ns = 0, help_chunks = 0, help_cmds = 0, help_ns = 0;
    uint64_t full = 0, full_ns = 0, inline_ns = 0, depth_max = 0, wakes = 0;
    uint64_t spliced = 0, spliced_cmds = 0;   // [NR-ZERO Z5] chunks handed over by splice(), their packets
};
struct WorkerStats {               // written under the queue mutex
    uint64_t chunks = 0, cmds = 0, ns = 0, sleeps = 0;
    uint64_t checked = 0, bad[NBMT_BAD_KINDS] = {}, bad_seq = 0;   // mode 2 (bad_seq: seq of the first bad chunk + 1)
};

// [DOS2] NR-CPU (docs/dos2/nr/NR-CPU.md): DM_NR_CPU=1 - which vkCmd* the replay spends its time in. Every packet is counted by op, one in
// 16 is timed (two steady_clock reads around its call). Written by the thread inside the chunk (one at a time), read by the report:
// relaxed atomics. Off: one not-taken branch a packet.
enum { NBMT_PROF_OPS = 64 };
struct Prof { std::atomic<uint64_t> calls[NBMT_PROF_OPS], timed[NBMT_PROF_OPS], ns[NBMT_PROF_OPS]; uint32_t tick = 0;
    Prof() { for (int i = 0; i < NBMT_PROF_OPS; i++) { calls[i] = 0; timed[i] = 0; ns[i] = 0; } } };
struct Queue {
    // configuration (set before the first begin, read-only afterwards)
    int mode = 0;
    bool prof_on = false;                  // [DOS2] NR-CPU DM_NR_CPU=1
    Prof prof;
    uint32_t chunk_bytes = 64 << 10, flush_cmds = 256, max_queued = 64;
    void (*on_worker_start)() = nullptr;   // thread name / affinity (engine side)
    bool test_corrupt = false;             // host test: flip a byte after the producer's hash
    // producer only
    void *cb = nullptr;
    Chunk *cur = nullptr;
    uint64_t seq_next = 0;
    Stats st;
    // shared (mu)
    std::mutex mu;
    std::condition_variable cv_work, cv_idle;
    Chunk *head = nullptr, *tail = nullptr, *free_list = nullptr;
    unsigned queued = 0, free_n = 0;
    bool busy = false, stop_flag = false, worker_waiting = false, producer_waiting = false, running = false;
    uint64_t seq_expect = 0;
    WorkerStats ws;
    std::thread *th = nullptr;     // heap: a joinable std::thread left at exit would terminate

    bool threaded() const { return mode >= 2; }

    // ---- replay (one thread at a time: mode 1 the producer; else whoever set `busy`) ----
    struct Verdict { uint32_t cmds = 0; int bad = -1; };
    Verdict replay(Chunk *c, bool check) {
        Verdict v;
        if (check && hash_bytes(c->data(), c->used) != c->hash) v.bad = NBMT_BAD_HASH_PRE;
        uint32_t at = 0;
        while (at < c->used) {
            const Pkt *p = reinterpret_cast<const Pkt *>(c->data() + at);
            if (check && (p->size < sizeof(Pkt) || (p->size & 7) || p->size > c->used - at || !p->run)) { if (v.bad < 0) v.bad = NBMT_BAD_WALK; break; }
            if (__builtin_expect(prof_on, 0)) {   // [DOS2] NR-CPU
                const unsigned op = p->op < NBMT_PROF_OPS ? p->op : 0;
                prof.calls[op].fetch_add(1, std::memory_order_relaxed);
                if (!(++prof.tick & 15)) { const uint64_t t0 = now_ns(); p->run(c->cb, p); prof.ns[op].fetch_add(now_ns() - t0, std::memory_order_relaxed); prof.timed[op].fetch_add(1, std::memory_order_relaxed); }
                else p->run(c->cb, p);
            } else p->run(c->cb, p);
            at += p->size; ++v.cmds;
        }
        if (check) {
            if (v.bad < 0 && v.cmds != c->cmds) v.bad = NBMT_BAD_WALK;
            if (v.bad < 0 && hash_bytes(c->data(), c->used) != c->hash) v.bad = NBMT_BAD_HASH_POST;
        }
        return v;
    }
    // under mu: the chunk is done
    void finish_locked(Chunk *c, const Verdict &v, bool check, uint64_t ns, bool by_worker) {
        if (check) {
            ++ws.checked;
            int bad = v.bad;
            if (c->seq != seq_expect && bad < 0) bad = NBMT_BAD_ORDER;
            if (bad >= 0) { ++ws.bad[bad]; if (!ws.bad_seq) ws.bad_seq = c->seq + 1; }
        }
        seq_expect = c->seq + 1;
        if (by_worker) { ++ws.chunks; ws.cmds += v.cmds; ws.ns += ns; }
        if (c->big || free_n > max_queued + 2) free(c);
        else { c->next = free_list; free_list = c; ++free_n; }
    }
    Chunk *pop_locked() { Chunk *c = head; head = c->next; if (!head) tail = nullptr; --queued; c->next = nullptr; return c; }

    void worker_loop() {
        if (on_worker_start) on_worker_start();
        std::unique_lock<std::mutex> l(mu);
        for (;;) {
            if (head && !busy) {
                Chunk *c = pop_locked(); busy = true; const bool check = mode == 2;
                l.unlock();
                const uint64_t t0 = now_ns(); const Verdict v = replay(c, check); const uint64_t ns = now_ns() - t0;
                l.lock();
                busy = false; finish_locked(c, v, check, ns, true);
                if (producer_waiting) cv_idle.notify_one();   // the flag is the producer's: it may have to wait for the next chunk too
                continue;
            }
            if (stop_flag) break;
            worker_waiting = true; ++ws.sleeps; cv_work.wait(l); worker_waiting = false;
        }
    }
    bool start() {                 // producer; idempotent
        if (!threaded()) return true;
        { std::lock_guard<std::mutex> l(mu); if (running) return true; stop_flag = false; }
#if defined(__cpp_exceptions)
        try { th = new std::thread([this] { worker_loop(); }); } catch (...) { th = nullptr; }
#else
        th = new std::thread([this] { worker_loop(); });
#endif
        if (!th) return false;
        std::lock_guard<std::mutex> l(mu); running = true; return true;
    }
    void stop() {                  // producer: drains, then joins the worker
        if (cb) end();
        std::thread *t = nullptr;
        { std::lock_guard<std::mutex> l(mu); if (!running) return; stop_flag = true; running = false; t = th; th = nullptr; }
        cv_work.notify_all();
        if (t) { t->join(); delete t; }
    }
    void trim() {                  // producer, queue idle: give the chunk memory back
        std::lock_guard<std::mutex> l(mu);
        while (free_list) { Chunk *c = free_list; free_list = c->next; free(c); } free_n = 0;
    }

    // ---- producer ----
    void begin(void *command_buffer) { cb = command_buffer; cur = nullptr; ++st.subs; }
    Chunk *take(uint32_t need) {
        const bool big = need > chunk_bytes;
        if (!big) {
            std::lock_guard<std::mutex> l(mu);
            if (free_list) { Chunk *c = free_list; free_list = c->next; --free_n; c->next = nullptr; c->used = c->cmds = 0; c->big = 0; return c; }
        }
        const uint32_t cap = big ? need : chunk_bytes;
        Chunk *c = static_cast<Chunk *>(malloc(sizeof(Chunk) + cap));
        if (!c) { ++st.alloc_fail; return nullptr; }
        ++st.mallocs; if (big) ++st.big;
        c->next = nullptr; c->cb = nullptr; c->seq = c->hash = 0; c->cap = cap; c->used = c->cmds = 0; c->big = big;
        return c;
    }
    // Room for one packet of `bytes` (8-aligned, header included) in the current chunk; nullptr = no memory (the caller drains
    // and calls directly). The packet and its array copies never straddle chunks.
    Pkt *open(uint32_t bytes) {
        if (cur && cur->cap - cur->used < bytes) flush();
        if (!cur) { cur = take(bytes); if (!cur) return nullptr; cur->cb = cb; }
        Pkt *p = reinterpret_cast<Pkt *>(cur->data() + cur->used);
        p->size = bytes; p->pad = 0;
        return p;
    }
    void close(Pkt *p) {
        cur->used += p->size; ++cur->cmds; ++st.cmds;
        if (cur->cmds >= flush_cmds) flush();
    }
    void flush(bool wake_worker = true) {   // hand the current chunk over (sync: no wake-up, the producer drains the queue itself)
        Chunk *c = cur; if (!c) return; cur = nullptr;
        if (!c->used) { std::lock_guard<std::mutex> l(mu); if (c->big) free(c); else { c->next = free_list; free_list = c; ++free_n; } return; }
        c->seq = seq_next++; ++st.chunks; st.bytes += c->used;
        if (mode == 2) { c->hash = hash_bytes(c->data(), c->used); if (test_corrupt) c->data()[c->used - 1] ^= 0x40; }
        if (!threaded()) {         // mode 1: replay here
            const uint64_t t0 = now_ns(); const Verdict v = replay(c, false); st.inline_ns += now_ns() - t0;
            std::lock_guard<std::mutex> l(mu); finish_locked(c, v, false, 0, false); return;
        }
        std::unique_lock<std::mutex> l(mu);
        c->next = nullptr; if (tail) tail->next = c; else head = c; tail = c; ++queued;
        if (queued > st.depth_max) st.depth_max = queued;
        if (queued >= max_queued) { ++st.full; const uint64_t t0 = now_ns(); drain_locked(l, max_queued / 2); st.full_ns += now_ns() - t0; }
        const bool wake = wake_worker && head && worker_waiting && !busy;
        l.unlock();
        if (wake) { ++st.wakes; cv_work.notify_one(); }
    }
    // under mu: replay / wait until at most `leave` chunks are queued (0: until the queue is empty and nobody is inside a chunk)
    // The worker, once awake, takes chunk after chunk without letting go (it pops the next one under the same lock hold), so the
    // producer replays here only what a sleeping worker would have to be woken for.
    void drain_locked(std::unique_lock<std::mutex> &l, unsigned leave) {
        for (;;) {
            if (leave ? queued <= leave : (!head && !busy)) return;
            if (head && !busy) {   // help: replay it here instead of sleeping
                Chunk *c = pop_locked(); busy = true; const bool check = mode == 2;
                l.unlock();
                const uint64_t t0 = now_ns(); const Verdict v = replay(c, check); const uint64_t ns = now_ns() - t0;
                l.lock();
                busy = false; finish_locked(c, v, check, ns, false);
                ++st.help_chunks; st.help_cmds += v.cmds; st.help_ns += ns;
                continue;
            }
            const uint64_t t0 = now_ns();
            producer_waiting = true; cv_idle.wait(l, [this] { return !busy; }); producer_waiting = false;
            const uint64_t ns = now_ns() - t0; ++st.waits; st.wait_ns += ns; if (ns > st.wait_max_ns) st.wait_max_ns = ns;
        }
    }
    // [DOS2] NR-ZERO Z5 (docs/dos2/nr/NR-ZERO.md section 14): a chunk of packets written apart from the queue (take()n from it, filled by
    // a builder - xbg_nb_mt.inc Build) is handed over whole AT THIS POSITION of the stream: everything the producer wrote before it is
    // replayed before it, everything written after it after it. The producer's current chunk is handed over first, then the chunk by
    // the same hand-over (sequence number, hash, mode 1 replay, queue full, wake-up) - the replay order is the hand-over order.
    void splice(Chunk *c) {
        if (!c) return;
        flush();
        c->cb = cb; st.cmds += c->cmds; ++st.spliced; st.spliced_cmds += c->cmds;
        cur = c; flush();
    }
    // A chunk taken for a builder and not handed over (its run was not used): back to the free list.
    void give_back(Chunk *c) {
        if (!c) return;
        std::lock_guard<std::mutex> l(mu);
        if (c->big) free(c); else { c->used = c->cmds = 0; c->next = free_list; free_list = c; ++free_n; }
    }
    // [DOS2] NR-ZERO Z5, the second writer (docs/dos2/nr/NR-ZERO.md section 18; dm/gfx/nr/nr_z5w.h). A thread that is NOT the producer
    // (the game thread) fills chunks of its own and hands them to the producer, which splices them. take_spare is its take(): the
    // free list under the queue's mutex (one lock a chunk, never a packet), no producer statistics (they are the producer's alone),
    // at most spare_max chunks out at a time (nullptr = none now: that run is not written ahead). A spare chunk ends in
    // splice_spare (the producer hands it over) or give_back_spare (any thread: its run was declined or never reached).
    uint32_t spare_max = 64;
    std::atomic<uint32_t> spare_out{0};
    std::atomic<uint64_t> spare_taken{0}, spare_mallocs{0}, spare_none{0};
    Chunk *take_spare() {
        if (spare_out.load(std::memory_order_relaxed) >= spare_max) { spare_none.fetch_add(1, std::memory_order_relaxed); return nullptr; }   // (one writer: nobody else adds)
        Chunk *c = nullptr;
        { std::lock_guard<std::mutex> l(mu); if (free_list) { c = free_list; free_list = c->next; --free_n; } }
        if (!c) {
            c = static_cast<Chunk *>(malloc(sizeof(Chunk) + chunk_bytes));
            if (!c) { spare_none.fetch_add(1, std::memory_order_relaxed); return nullptr; }
            spare_mallocs.fetch_add(1, std::memory_order_relaxed); c->cap = chunk_bytes;
        }
        c->next = nullptr; c->cb = nullptr; c->seq = c->hash = 0; c->used = c->cmds = 0; c->big = 0;
        spare_out.fetch_add(1, std::memory_order_relaxed); spare_taken.fetch_add(1, std::memory_order_relaxed);
        return c;
    }
    void splice_spare(Chunk *c) { if (!c) return; spare_out.fetch_sub(1, std::memory_order_relaxed); splice(c); }                 // producer
    void give_back_spare(Chunk *c) { if (!c) return; spare_out.fetch_sub(1, std::memory_order_relaxed); give_back(c); }           // any thread
    // Everything handed over so far is in the command buffer when this returns; the producer may then use it directly.
    void sync(int why) {
        ++st.sync[why]; flush(false);
        if (!threaded()) return;
        std::unique_lock<std::mutex> l(mu); drain_locked(l, 0);
    }
    void end() { sync(NBMT_SY_SUBMIT); cb = nullptr; }
};

} // namespace nbmtq
#endif
