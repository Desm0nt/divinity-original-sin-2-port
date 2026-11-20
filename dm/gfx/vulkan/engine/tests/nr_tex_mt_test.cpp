// [dos2-nr05] NR-05: the material-texture core under the front end's locking (dm/host/hle_dos2_nr.c): one mutex around every call but
// nr_tex_job_run; a loader thread makes textures (create events) and sometimes rewrites one whole storage later (a late streamer
// write) or finalizes it (gone); one worker runs jobs; a translator thread takes blobs (first upload) and keys, checks every handed-out
// blob against a fresh nvn_deswizzle of the storage as it is at that moment, and drops it. At the end: no blob held, budget back to 0.
// [dos2-nr22] argument "ts": the same with an NR-22 (DM_NR_TEXSTREAM) sink - fake engine objects (staging of their own) for every
// format, ASTC objects for about half of the cacheable textures (their "blocks" = a pattern of the key), released objects queued under
// a lock of their own and destroyed by the worker between jobs (as xbg_nrts_idle). Checks: a handed-out object's staging equals the
// storage (deswizzled) / the key's pattern at take time, every object ends taken or released exactly once, nothing is left.
// Build (Windows clang 21 or WSL g++), from this folder:
//   clang -std=c11 -O2 -c ../../../nr/nr_tex.c ../../../tex/nvn_deswizzle.c ../../../nvn/nvn_tex.c && clang++ -std=c++17 -O2 -Wall nr_tex_mt_test.cpp nr_tex.o nvn_deswizzle.o nvn_tex.o -o nr_tex_mt_test
extern "C" {   // nvn_tex.h / nvn_deswizzle.h carry no C++ guards
#include "../../../nr/nr_tex.h"
#include "../../../tex/nvn_deswizzle.h"
}
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <algorithm>
#include <chrono>
#include <vector>

static std::mutex mu;
static std::condition_variable cv;
static std::atomic<bool> stop{false};
static nr_tex_core *core;
struct Tex { nvn_texdesc d; nvn_texlayout L; std::vector<uint8_t> mem; uint64_t serial; std::atomic<int> state{0}; };   // 0 alive, 1 gone
static std::vector<Tex *> texs;
static std::atomic<size_t> made{0};   // create events so far (the translator uploads a texture some frames after its creation)
static std::atomic<uint64_t> handed{0}, verified{0}, bad{0}, misses{0}, rewrites{0}, gones{0}, keys{0};
static std::mutex mem_mu;   // storage writes vs. the translator's check (the real engine has no such lock: the probe stands in)

static int key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) { *crc = (uint32_t)nr_tex_hash(mem, size); *adler = (uint32_t)(size * 7); return 0; }
// ---- NR-22 fake sink ----
struct Obj { int kind; uint64_t bytes; std::vector<uint8_t> staging; uint32_t crc; std::atomic<int> st{0}; };   // st: 0 out, 1 taken, 2 released
static bool ts_mode;
static std::mutex grave_mu; static std::vector<Obj *> grave, all_objs; static std::mutex all_mu;
static std::atomic<uint64_t> o_made{0}, o_astc{0}, o_released{0}, o_taken{0}, o_double{0}, o_destroyed{0};
static void fill_astc(uint8_t *p, uint64_t n, uint32_t crc) { for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)(crc >> (8 * (i & 3))) ^ (uint8_t)i; }
static void *ts_make(void *, const nrt_src *, uint64_t bytes, uint8_t **dst) {
    Obj *o = new Obj; o->kind = NRT_TS_DESW; o->bytes = bytes; o->staging.assign((size_t)bytes, 0xcd); *dst = o->staging.data(); ++o_made;
    { std::lock_guard<std::mutex> l(all_mu); all_objs.push_back(o); } return o; }
static void *ts_make_astc(void *, const nrt_src *s, uint32_t crc, uint32_t, uint64_t *bytes) {
    if (crc & 1) return nullptr;   // "no entry"
    Obj *o = new Obj; o->kind = NRT_TS_ASTC; o->bytes = s->L.size; o->crc = crc; o->staging.resize((size_t)o->bytes); fill_astc(o->staging.data(), o->bytes, crc); *bytes = o->bytes; ++o_astc;
    { std::lock_guard<std::mutex> l(all_mu); all_objs.push_back(o); } return o; }
static void ts_release(void *, void *p) { Obj *o = (Obj *)p; int z = 0; if (!o->st.compare_exchange_strong(z, 2)) { ++o_double; return; } ++o_released; std::lock_guard<std::mutex> l(grave_mu); grave.push_back(o); }
static void ts_idle() { std::vector<Obj *> g; { std::lock_guard<std::mutex> l(grave_mu); g.swap(grave); } for (Obj *o : g) { o->staging.clear(); o->staging.shrink_to_fit(); ++o_destroyed; } }

static void worker() {
    static nrt_job j;
    std::unique_lock<std::mutex> l(mu);
    while (!stop) {
        if (ts_mode) { l.unlock(); ts_idle(); l.lock(); }
        if (!nr_tex_job_begin(core, &j)) { nr_tex_expire(core, (uint64_t)-1 / 2, (uint64_t)-1); cv.wait_for(l, std::chrono::milliseconds(2)); continue; }
        l.unlock();
        { std::lock_guard<std::mutex> m(mem_mu); nr_tex_job_run(&j, key); }
        l.lock();
        nr_tex_job_end(core, &j, 1);
    }
}

int main(int argc, char **argv) {
    ts_mode = argc > 1 && !strcmp(argv[1], "ts");
    core = nr_tex_new(8u << 20, 1024, 2u << 20);
    core->want_key = 1;
    const int fmts[] = {0x25, 0x42, 0x46, 0x48, 0x29, 0x01};
    if (ts_mode) { const nrt_sink sk = {ts_make, ts_make_astc, ts_release, nullptr}; nr_tex_set_sink(core, &sk); for (int f : fmts) nr_tex_verdict(core, (unsigned)f, 1); }
    std::atomic<uint64_t> ts_handed{0}, ts_astc_handed{0};
    unsigned seed = 7;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 600; i++) {
        Tex *t = new Tex; memset(&t->d, 0, sizeof t->d);
        t->d.target = 1; t->d.format = fmts[i % 6]; t->d.width = 32u << (rnd() % 5); t->d.height = 32u << (rnd() % 4); t->d.depth = 1; t->d.levels = 1 + rnd() % 5;
        nvn_tex_layout(&t->d, &t->L); t->mem.resize((size_t)t->L.size); for (auto &b : t->mem) b = (uint8_t)rnd();
        t->serial = 1000 + (uint64_t)i;
        texs.push_back(t);
    }
    std::thread w(worker);
    std::thread loader([&] {
        unsigned s2 = 99;
        for (size_t i = 0; i < texs.size(); i++) {
            Tex *t = texs[i];
            nrt_src s; memset(&s, 0, sizeof s); s.d = t->d; s.L = t->L; s.mem = t->mem.data(); s.serial = t->serial; s.src = 4;
            { std::lock_guard<std::mutex> l(mu); nr_tex_create(core, &s, 0, 1, 1); }
            cv.notify_one(); made = i + 1;
            s2 = s2 * 1103515245u + 12345u;
            if (i > 10 && (s2 >> 16) % 7 == 0) {   // a late write into an older texture: the whole storage
                Tex *o = texs[i - 1 - (s2 >> 8) % 10];
                std::lock_guard<std::mutex> m(mem_mu); for (auto &b : o->mem) b = (uint8_t)(b * 3 + 1); ++rewrites;
            }
            if (i > 20 && (s2 >> 12) % 11 == 0) {   // finalized
                Tex *o = texs[i - 1 - (s2 >> 4) % 20]; int z = 0;
                if (o->state.compare_exchange_strong(z, 1)) { std::lock_guard<std::mutex> l(mu); nr_tex_gone(core, o->serial); ++gones; }
            }
            std::this_thread::yield();
        }
    });
    std::thread translator([&] {
        for (int pass = 0; pass < 3; pass++)
            for (size_t i = 0; i < texs.size(); i++) {
                while (pass == 0 && made < std::min(texs.size(), i + 24)) std::this_thread::sleep_for(std::chrono::microseconds(200));
                Tex *t = texs[i];
                std::lock_guard<std::mutex> m(mem_mu);   // the storage is stable while the translator copies (as on a real first upload)
                uint64_t n = 0, probe = 0; int why = 0; uint8_t *b; void *ts = nullptr; int kind = 0;
                if (ts_mode) { std::lock_guard<std::mutex> l(mu); if (!nr_tex_take_ts(core, t->serial, t->mem.data(), t->L.size, &b, &ts, &kind, &n, &probe, &why)) b = nullptr; }
                else { std::lock_guard<std::mutex> l(mu); b = nr_tex_take(core, t->serial, t->mem.data(), t->L.size, &n, &probe, &why); }
                if (ts) {   // NR-22: the engine's object; the translator owns it now
                    Obj *o = (Obj *)ts; int z = 0; if (!o->st.compare_exchange_strong(z, 1)) ++o_double; else ++o_taken;
                    bool same = probe == nr_tex_probe(t->mem.data(), t->L.size) && n == o->bytes && kind == o->kind;
                    if (kind == NRT_TS_DESW) {
                        nrt_sub sub[NRT_SUBS]; uint64_t tot = 0; const int k = nr_tex_layout(&t->d, &t->L, sub, NRT_SUBS, &tot); same = same && tot == n && b == o->staging.data();
                        for (int q = 0; same && q < k; q++) { std::vector<uint8_t> want((size_t)sub[q].bytes); nvn_deswizzle(&t->d, sub[q].level, sub[q].layer, t->mem.data(), want.data(), sub[q].pitch);
                            same = !memcmp(want.data(), b + sub[q].off, want.size()); }
                    } else { uint32_t ec, ea; key(t->mem.data(), t->L.size, &ec, &ea); std::vector<uint8_t> want((size_t)n); fill_astc(want.data(), n, ec); same = same && !b && o->crc == ec && !memcmp(want.data(), o->staging.data(), (size_t)n); ++ts_astc_handed; }
                    ++(same ? verified : bad); ++ts_handed;
                    std::lock_guard<std::mutex> l(mu); nr_tex_unhold(core, n);
                } else if (b) {
                    ++handed;
                    nrt_sub sub[NRT_SUBS]; uint64_t tot = 0; const int k = nr_tex_layout(&t->d, &t->L, sub, NRT_SUBS, &tot);
                    bool same = tot == n && probe == nr_tex_probe(t->mem.data(), t->L.size);
                    for (int q = 0; same && q < k; q++) {
                        std::vector<uint8_t> want((size_t)sub[q].bytes);
                        nvn_deswizzle(&t->d, sub[q].level, sub[q].layer, t->mem.data(), want.data(), sub[q].pitch);
                        same = !memcmp(want.data(), b + sub[q].off, want.size());
                    }
                    ++(same ? verified : bad);
                    std::lock_guard<std::mutex> l(mu); nr_tex_drop(core, b, n);
                } else ++misses;
                uint32_t c = 0, a = 0;
                { std::lock_guard<std::mutex> l(mu); if (nr_tex_key(core, t->serial, t->mem.data(), t->L.size, &c, &a)) {
                    uint32_t ec, ea; key(t->mem.data(), t->L.size, &ec, &ea); if (c != ec || a != ea) ++bad; else ++keys; } }
                cv.notify_one();
            }
    });
    loader.join(); translator.join();
    stop = true; cv.notify_all(); w.join();
    { std::lock_guard<std::mutex> l(mu); for (Tex *t : texs) nr_tex_gone(core, t->serial); }
    char line[1200]; nr_tex_summary(core, line, sizeof line);
    const uint64_t live = core->st.live_bytes;
    printf("handed %llu (verified %llu, bad %llu), misses %llu, keys %llu, late rewrites %llu, finalized %llu\n%s\n", (unsigned long long)handed, (unsigned long long)verified,
           (unsigned long long)bad, (unsigned long long)misses, (unsigned long long)keys, (unsigned long long)rewrites, (unsigned long long)gones, line);
    nr_tex_free(core);
    if (ts_mode) {
        ts_idle();
        uint64_t open = 0; { std::lock_guard<std::mutex> l(all_mu); for (Obj *o : all_objs) if (o->st.load() == 0) ++open; }
        const uint64_t mk = o_made + o_astc;
        printf("NR-22 sink: objects made %llu (deswizzled %llu, ASTC %llu), taken %llu (handed %llu, ASTC %llu), released %llu (destroyed by the worker / at the end %llu), double ends %llu, never ended %llu\n",
               (unsigned long long)mk, (unsigned long long)o_made.load(), (unsigned long long)o_astc.load(), (unsigned long long)o_taken.load(), (unsigned long long)ts_handed.load(), (unsigned long long)ts_astc_handed.load(),
               (unsigned long long)o_released.load(), (unsigned long long)o_destroyed.load(), (unsigned long long)o_double.load(), (unsigned long long)open);
        if (o_double || open || o_taken + o_released != mk || o_destroyed != o_released || !ts_handed || !ts_astc_handed) { printf("FAIL: nr_tex_mt ts: object accounting\n"); return 1; }
    }
    for (Tex *t : texs) delete t;
    if (bad || live || !verified) { printf("FAIL: nr_tex_mt (bad %llu, blob bytes still held %llu, verified %llu)\n", (unsigned long long)bad, (unsigned long long)live, (unsigned long long)verified); return 1; }
    printf("PASS: nr_tex_mt%s: every handed-out blob%s and key equals the storage at take time; no blob held at the end\n", ts_mode ? " ts" : "", ts_mode ? ", NR-22 object (each taken or released exactly once)" : "");
    return 0;
}
