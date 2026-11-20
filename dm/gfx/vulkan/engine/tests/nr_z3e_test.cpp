// [DOS2] NR-ZERO Z3b / Z3c / Z3d host test (dm/gfx/nr/nr_z3e.h; docs/dos2/nr/NR-ZERO.md section 13): the epochs that stand for a walk.
//   1  Z3b, every writer alone: an entry stamped quiet; ONE field of ONE of its images is written so that nr_dimg_skip (nr_desc.h) no
//      longer answers SKIP - the word must not stand (each of the seven fields, the image destroyed, the submission, the slot serial);
//      a write that keeps the image quiet must not move the epoch (else the word would never stand on a real frame)
//   2  Z3b, model against word on a random stream (2 M steps: field writes of every kind, walks, submissions, slot serials, images
//      destroyed and made again at the same address): whenever the word of an entry stands, the walk finds every image SKIP and
//      already marked with this slot serial
//   3  Z3c, model against table: a model of snapshot() for vertex / index ranges - zero-copy chunks (live, retired, mapped again,
//      written by the replay, the sticky class moving with the slot serial), static copies (demoted, trimmed, the snapshot epoch),
//      pending writebacks - answers every ask; the table in front of it, stamped as the engine stamps it, must give the same view
//      whenever it stands (2 M asks, a small table so that ranges collide)
//   4  Z3d, the ring word against the page walk (nb_ubo_view of nb_hit.h) on random chunks, sticky maps, rings, alignments and blocks:
//      a block the word answers is served by the walk at the same offset; a page of the ring turning sticky ends the word
// The mutation checks - this test must FAIL when built with
//   5  [NR-ZERO Z8 images, NR-ZERO.md section 29] the read word against a model of the walk with its read path (get_image's check,
//      prepare_read: pending clear, attachment of the open pass, layout, written since the barrier the read sees - full or
//      fragment-only -, the pass ended and the barrier recorded) on a random stream of draws of entries inside passes, pass changes,
//      storage writes, copies, pending clears, barriers, guest writes, submissions: whenever the word of an entry stands, the walk
//      would have done nothing - no image leaves SKIP / "already read", no pass is ended, no barrier, no clear lands
//   -DNRZE_MUTATE=k    k = 1..27: one bump / one compared word of nr_z3e.h dropped
//   -DNRZE_MODEL_DROP=k k = 8..9: [Z8 images] the model's engine forgets 8 the count of pass marks at a draw that continues its pass,
//                      9 the question "is a clear pending" before it asks the word; k = 1..7: the MODEL forgets to move one epoch where the engine moves it (1 the image epoch at ~Image, 2
//                      zc::live_ep at a retire / a new chunk, 3 the chunk's wr_ver at a replay write, 4 the pending-writeback generation,
//                      5 nrbuf::st_ep at a demote / trim, 6 the snapshot epoch, 7 the chunk's st_ver at a page turning sticky) - what a
//                      writer without its bump would do on the phone
// Build (Windows LLVM or any C++17 compiler), from the repository root:
//   clang++ -std=c++17 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_z3e_test.cpp -o nr_z3e_test && ./nr_z3e_test
//   for k in $(seq 1 16); do clang++ -std=c++17 -O2 -D_CRT_SECURE_NO_WARNINGS -DNRZE_MUTATE=$k dm/gfx/vulkan/engine/tests/nr_z3e_test.cpp -o nr_z3e_mut && ! ./nr_z3e_mut >/dev/null || echo "mutation $k NOT caught"; done
//   for k in $(seq 1 7); do clang++ -std=c++17 -O2 -D_CRT_SECURE_NO_WARNINGS -DNRZE_MODEL_DROP=$k dm/gfx/vulkan/engine/tests/nr_z3e_test.cpp -o nr_z3e_mut && ! ./nr_z3e_mut >/dev/null || echo "dropped bump $k NOT caught"; done
#include "../../../nr/nr_z3e.h"
#include "../../../nr/nr_desc.h"
#include "../../../nb/nb_hit.h"
#include <stdio.h>
#include <new>
#include <vector>
#ifndef NRZE_MODEL_DROP
#define NRZE_MODEL_DROP 0
#endif

static long fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (fails < 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
static uint64_t rs = 0x243F6A8885A308D3ull;
static uint64_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static unsigned rn(unsigned n) { return unsigned(rnd() % n); }

// ---------------- Z3b ----------------
static uint64_t sub_now = 5, serial_now = 9;
uint64_t nrz_sub_now() { return sub_now; }
enum { L_UNDEF = 0, L_GENERAL = 1, L_XFER = 6, L_ATT = 2 };
struct Img {   // the fields of the engine's Image that nr_dimg_skip reads, with the engine's types
    nrz_leave<int, L_GENERAL> layout{L_UNDEF};
    nrz_fall uploaded{false};
    nrz_rise<bool> gpu_written{false};
    nrz_rise<uint64_t> storage_write{0}, last_write{0}, opw{0};
    nrz_checked checked_submission{UINT64_MAX};
    uint64_t slot_use = 0;
    ~Img() { if (NRZE_MODEL_DROP != 1) ++nrz_img_ep; }
};
static bool hook = false;
static int skip_of(const Img &im) {
    const nr_dimg_in x{im.last_write, im.storage_write, im.opw, uint8_t(im.checked_submission == sub_now), uint8_t(im.uploaded), uint8_t(im.gpu_written), uint8_t(im.layout == L_GENERAL), uint8_t(hook)};
    return nr_dimg_skip(&x);
}
static void make_quiet(Img &im) { im.gpu_written = false; im.last_write = 0; im.storage_write = 0; im.opw = 0; im.uploaded = true; im.layout = L_GENERAL; im.checked_submission = sub_now; }
struct Ent { std::vector<Img *> img; nrz_quiet quiet{}; };
// the engine's walk (serve() / nrz3::draw): returns the images sent to the read path; all SKIP and no hook: the word is set
static unsigned walk(Ent &e) {
    unsigned nrd = 0;
    for (Img *im : e.img) { if (skip_of(*im) == NRD_I_SKIP) { im->slot_use = serial_now; continue; } ++nrd; }
    if (!nrd && !hook) nrz_quiet_set(&e.quiet, nrz_img_ep, sub_now, serial_now);
    return nrd;
}
static bool stands(const Ent &e) { return !hook && nrz_quiet_ok(&e.quiet, nrz_img_ep, sub_now, serial_now); }

static void z3b_each() {
    for (int w = 0; w < 10; w++) {
        alignas(Img) unsigned char mem[3][sizeof(Img)]; Img *a[3];
        for (int i = 0; i < 3; i++) { a[i] = new (mem[i]) Img(); make_quiet(*a[i]); }
        Ent e; e.img = {a[0], a[1], a[2]};
        CHECK(!stands(e)); CHECK(walk(e) == 0); CHECK(stands(e));
        const uint64_t ep0 = nrz_img_ep;
        // writes that keep every image quiet do not move the epoch
        a[1]->gpu_written = false; a[1]->last_write = 0; a[1]->uploaded = true; a[1]->layout = L_GENERAL; a[1]->checked_submission = sub_now; a[1]->opw = 0; a[1]->storage_write = 0;
        CHECK(nrz_img_ep == ep0); CHECK(stands(e));
        Img &t = *a[w % 3];
        switch (w) {
        case 0: t.gpu_written = true; break;
        case 1: t.last_write = 77; break;
        case 2: t.storage_write = 3; break;
        case 3: t.opw = 1; break;
        case 4: t.uploaded = false; break;
        case 5: t.layout = L_XFER; break;
        case 6: t.checked_submission = UINT64_MAX; break;
        case 7: ++sub_now; break;
        case 8: ++serial_now; break;
        case 9: t.~Img(); new (&t) Img(); break;   // destroyed, another image at the same address (not uploaded, not checked)
        }
        bool all = true; for (Img *im : e.img) all = all && skip_of(*im) == NRD_I_SKIP && im->slot_use == serial_now;
        CHECK(!all);                 // the write did end "quiet" / "already marked"
        CHECK(!stands(e));           // so the word must not stand
        for (int i = 0; i < 3; i++) a[i]->~Img();
    }
    // a copy assignment of a field goes through the same rule
    { Img a, b; make_quiet(a); b.gpu_written = true; const uint64_t ep0 = nrz_img_ep; a.gpu_written = b.gpu_written; CHECK(nrz_img_ep != ep0); }
    { Img a, b; make_quiet(a); make_quiet(b); const uint64_t ep0 = nrz_img_ep; a.layout = b.layout; a.uploaded = b.uploaded; a.checked_submission = b.checked_submission; CHECK(nrz_img_ep == ep0); }
    // a write of checked_submission that is not "leaving this submission" does not move it (an image checked for the first time)
    { Img a; const uint64_t ep0 = nrz_img_ep; a.checked_submission = sub_now; CHECK(nrz_img_ep == ep0); a.checked_submission = sub_now + 1; CHECK(nrz_img_ep != ep0); }
}
static void z3b_random() {
    enum { NI = 48, NE = 40 };
    static unsigned char mem[NI][sizeof(Img)]; Img *im[NI];
    for (int i = 0; i < NI; i++) { im[i] = new (mem[i]) Img(); }
    static Ent ent[NE];
    for (auto &e : ent) { unsigned n = 1 + rn(5); e.img.clear(); for (unsigned k = 0; k < n; k++) e.img.push_back(im[rn(NI)]); e.quiet = nrz_quiet{}; }
    long stood = 0, asked = 0;
    for (long step = 0; step < 2000000; step++) {
        const unsigned op = rn(40) ? rn(600) : rn(10) < 8 ? 600 + rn(160) : rn(1000);   // mostly draws (between two events an entry is drawn several times, as in a frame); most events are a texture's first use
        Img &t = *im[rn(NI)];
        if (op < 600) {   // a draw of an entry: the word, then the walk the word stands for
            Ent &e = ent[rn(NE)]; ++asked;
            if (stands(e)) { ++stood; bool all = true; for (Img *x : e.img) all = all && skip_of(*x) == NRD_I_SKIP && x->slot_use == serial_now; CHECK(all); }
            walk(e);
        }
        else if (op < 760) { make_quiet(t); t.slot_use = serial_now; }                       // get_image: uploaded and checked now
        else if (op < 800) { t.last_write = rnd() | 1; t.gpu_written = true; }              // an attachment of a pass
        else if (op < 815) t.storage_write = rnd() | 1;
        else if (op < 825) t.opw = rnd() | 1;
        else if (op < 850) { t.uploaded = false; t.checked_submission = UINT64_MAX; }       // a guest write over the texture
        else if (op < 870) t.layout = rn(2) ? L_XFER : L_ATT;
        else if (op < 890) t.layout = L_GENERAL;
        else if (op < 905) { t.gpu_written = false; t.uploaded = false; t.checked_submission = UINT64_MAX; }
        else if (op < 925) t.checked_submission = rn(2) ? sub_now : sub_now - 1;
        else if (op < 940) { t.last_write = 0; t.storage_write = 0; t.opw = 0; }
        else if (op < 955) { t.~Img(); new (&t) Img(); if (rn(2)) make_quiet(t); }           // destroyed and made again at the same address
        else if (op < 975) ++serial_now;
        else if (op < 990) ++sub_now;
        else if (op < 995) hook = !hook;
        else { Ent &e = ent[rn(NE)]; unsigned n = 1 + rn(5); e.img.clear(); for (unsigned k = 0; k < n; k++) e.img.push_back(im[rn(NI)]); e.quiet = nrz_quiet{}; }   // the entry remade
    }
    hook = false;
    CHECK(stood * 20 > asked);   // the word does stand often enough for the test to mean something
    printf("  Z3b random: %ld draws asked, the word stood %ld\n", asked, stood);
    for (int i = 0; i < NI; i++) im[i]->~Img();
}

// ---------------- [NR-ZERO Z8 images] the read word ----------------
namespace z8i {
struct RImg : Img { bool pending = false; };
struct REnt { std::vector<RImg *> img; nrz_rtq q{}; bool frag = false; };
static uint64_t write_seq = 1, pm = 0, barrier_seq = 0, frag_seq = 0, pass_id = 0, fb_id = 0; static std::vector<RImg *> open; static unsigned pclear_n = 0;
static long ended = 0, barriers = 0, landed = 0, checked_n = 0;
static bool in_open(const RImg *im) { for (RImg *a : open) if (a == im) return true; return false; }
static bool written(const RImg &im, bool frag) { const uint64_t seen = frag ? frag_seq : barrier_seq; return uint64_t(im.last_write) > seen || uint64_t(im.storage_write) > seen; }
static void end_pass() { if (!open.empty()) { open.clear(); pass_id = 0; fb_id = 0; ++ended; } }
// get_image(t, true) + prepare_read: returns whether anything was done
static bool read_path(RImg &im, bool frag) {
    bool did = false;
    if (!(im.checked_submission == sub_now && (im.uploaded || im.gpu_written))) { im.uploaded = true; im.checked_submission = sub_now; did = true; ++checked_n; }
    im.slot_use = serial_now;
    if (im.pending) { im.pending = false; --pclear_n; im.last_write = ++write_seq; im.gpu_written = true; did = true; ++landed; }
    const bool io = in_open(&im), wr = written(im, frag);
    if (im.layout != L_GENERAL || wr || io) { end_pass(); im.layout = L_GENERAL; if (frag) frag_seq = write_seq; else barrier_seq = frag_seq = write_seq; ++barriers; did = true; }
    return did;
}
static bool ask(const REnt &e) { return (NRZE_MODEL_DROP == 9 || !pclear_n) && !hook && nrz_rtq_ok(&e.q, nrz_img_ep, sub_now, serial_now, write_seq - pm, pass_id, fb_id, e.frag); }
// the walk of an answered draw (the engine's loop) and, after it, the word. Returns whether the walk did anything.
static bool walk(REnt &e) {
    bool did = false;
    for (RImg *im : e.img) { if (skip_of(*im) == NRD_I_SKIP) { if (im->slot_use != serial_now) did = true; im->slot_use = serial_now; continue; } did = read_path(*im, e.frag) || did; }
    if (open.empty() || hook || pclear_n) return did;   // (the pass was ended: the draw is not answered; a hook; a pending clear)
    for (RImg *im : e.img) { const int k = skip_of(*im);
        const nrz_rt_in x{uint8_t(k == NRD_I_SKIP), uint8_t(k == NRD_I_RT), uint8_t(im->checked_submission == sub_now), uint8_t(im->uploaded || im->gpu_written), uint8_t(im->layout == L_GENERAL), uint8_t(in_open(im)), uint8_t(written(*im, e.frag)), uint8_t(im->pending)};
        if (!nrz_rt_img_ok(&x)) return did; }
    nrz_rtq_set(&e.q, nrz_img_ep, sub_now, serial_now, write_seq - pm, pass_id, fb_id, e.frag);
    return did;
}
static void pass_marks() { for (RImg *a : open) { a->last_write = ++write_seq; a->gpu_written = true; } if (NRZE_MODEL_DROP != 8) pm += open.size(); }   // a draw that continues its pass
static void random() {
    enum { NI = 40, NE = 36 };
    static unsigned char mem[NI][sizeof(RImg)]; RImg *im[NI]; for (int i = 0; i < NI; i++) im[i] = new (mem[i]) RImg();
    static REnt ent[NE]; uint64_t next_pass = 100;
    auto remake = [&](REnt &e) { unsigned n = 1 + rn(4); e.img.clear(); for (unsigned k = 0; k < n; k++) e.img.push_back(im[rn(NI)]); e.q = nrz_rtq{}; e.frag = rn(4) == 0; };
    for (auto &e : ent) remake(e);
    auto begin_pass = [&] { end_pass(); const unsigned n = 1 + rn(3); for (unsigned k = 0; k < n; k++) { RImg *a = im[rn(NI)]; if (!in_open(a)) open.push_back(a); }
        for (RImg *a : open) { if (a->pending) { a->pending = false; --pclear_n; } a->layout = L_GENERAL; a->last_write = ++write_seq; a->gpu_written = true; a->uploaded = true; a->checked_submission = sub_now; }   // the slow path: marks that are not pass marks
        pass_id = ++next_pass; fb_id = next_pass * 3; };
    long stood = 0, asked = 0, rt_stood = 0;
    for (long step = 0; step < 3000000; step++) {
        const unsigned op = rn(30) ? rn(600) : 600 + rn(400);
        RImg &t = *im[rn(NI)];
        if (op < 600) {   // an engine draw of an entry
            if (open.empty()) begin_pass();
            REnt &e = ent[rn(NE)]; ++asked;
            const bool st = ask(e);
            if (st) { ++stood; bool rt = false; for (RImg *x : e.img) rt = rt || skip_of(*x) != NRD_I_SKIP; rt_stood += rt;
                const long e0 = ended, b0 = barriers, l0 = landed, c0 = checked_n; const uint64_t ep0 = nrz_img_ep, ws0 = write_seq;
                const bool did = walk(e);   // the walk the word stood for
                CHECK(!did && e0 == ended && b0 == barriers && l0 == landed && c0 == checked_n && ep0 == nrz_img_ep && ws0 == write_seq); }
            else walk(e);
            if (!open.empty()) pass_marks(); else begin_pass();   // answered: the pass marks; else the draw goes the long way (a new pass)
        }
        else if (op < 640) begin_pass();
        else if (op < 660) end_pass();
        else if (op < 700) { t.storage_write = ++write_seq; t.gpu_written = true; }                 // a storage image written by a draw
        else if (op < 730) { end_pass(); t.layout = L_XFER; t.last_write = ++write_seq; t.gpu_written = true; if (rn(2)) t.layout = L_GENERAL; }   // a copy / blit into the image
        else if (op < 760) { if (!t.pending && !in_open(&t)) { t.pending = true; ++pclear_n; t.gpu_written = true; } }   // a clear left pending (no write mark until it lands)
        else if (op < 790) { end_pass(); if (rn(2)) frag_seq = write_seq; else barrier_seq = frag_seq = write_seq; }
        else if (op < 830) { make_quiet(t); t.slot_use = serial_now; if (t.pending) { t.pending = false; --pclear_n; } }   // a texture made again / uploaded: quiet
        else if (op < 860) { t.uploaded = false; t.checked_submission = UINT64_MAX; }           // a guest write over the texture
        else if (op < 880) t.checked_submission = rn(2) ? sub_now : sub_now - 1;
        else if (op < 900) { if (t.pending) { t.pending = false; --pclear_n; } t.~RImg(); new (&t) RImg(); if (rn(2)) make_quiet(t); for (auto &a : open) if (a == &t) { end_pass(); break; } }
        else if (op < 930) { end_pass(); ++serial_now; }
        else if (op < 960) { end_pass(); ++sub_now; ++serial_now; barrier_seq = frag_seq = 0; }
        else if (op < 965) hook = !hook;
        else if (op < 985) pass_marks();                                                           // a draw of something else that continues the pass
        else remake(ent[rn(NE)]);
    }
    hook = false;
    CHECK(stood * 10 > asked && rt_stood * 40 > asked);   // the word stands, and stands for entries with GPU-written images
    printf("  Z8 images random: %ld draws asked, the read word stood %ld (%ld of them for an entry with a GPU-written image); passes ended by a read %ld, barriers %ld, clears landed %ld\n", asked, stood, rt_stood, ended, barriers, landed);
    for (int i = 0; i < NI; i++) im[i]->~RImg();
    open.clear();
}
static void each() {
    nrz_rtq q{}; CHECK(!nrz_rtq_ok(&q, 0, 0, 0, 0, 0, 0, 0)); nrz_rtq_set(&q, 3, 4, 5, 6, 7, 8, 1); CHECK(nrz_rtq_ok(&q, 3, 4, 5, 6, 7, 8, 1));
    CHECK(!nrz_rtq_ok(&q, 9, 4, 5, 6, 7, 8, 1)); CHECK(!nrz_rtq_ok(&q, 3, 9, 5, 6, 7, 8, 1)); CHECK(!nrz_rtq_ok(&q, 3, 4, 9, 6, 7, 8, 1)); CHECK(!nrz_rtq_ok(&q, 3, 4, 5, 9, 7, 8, 1));
    CHECK(!nrz_rtq_ok(&q, 3, 4, 5, 6, 9, 8, 1)); CHECK(!nrz_rtq_ok(&q, 3, 4, 5, 6, 7, 9, 1)); CHECK(!nrz_rtq_ok(&q, 3, 4, 5, 6, 7, 8, 0));
    const nrz_rt_in good{0, 1, 1, 1, 1, 0, 0, 0}; CHECK(nrz_rt_img_ok(&good)); const nrz_rt_in sk{1, 0, 0, 0, 0, 1, 1, 1}; CHECK(nrz_rt_img_ok(&sk));
    nrz_rt_in x = good; x.rt = 0; CHECK(!nrz_rt_img_ok(&x)); x = good; x.checked = 0; CHECK(!nrz_rt_img_ok(&x)); x = good; x.content = 0; CHECK(!nrz_rt_img_ok(&x)); x = good; x.general = 0; CHECK(!nrz_rt_img_ok(&x));
    x = good; x.in_open = 1; CHECK(!nrz_rt_img_ok(&x)); x = good; x.written = 1; CHECK(!nrz_rt_img_ok(&x)); x = good; x.pending = 1; CHECK(!nrz_rt_img_ok(&x));
}
}

// ---------------- Z3c ----------------
// The model of snapshot() for a vertex / index range. Zero-copy: the chunk holding the address, when live, serves it in place unless a
// replay write made its bytes "recent" (the class of NR-TRX: a function of the chunk's written ranges and the slot serial) or a
// writeback is pending over it. Static copies: a buffer of the table, when ready, serves it from its copy.
enum { MC = 6, MB = 24, CHUNK = 1 << 16 };
struct MChunk { bool live; uint64_t gen, buf, wr_ver, wr_serial[16]; };   // wr_serial[k]: slot serial of the last replay write onto sixteenth k (0 = never)
struct MStat { bool on, demoted; uint64_t lo, n, copy_buf, copy_off; };
static MChunk mc[MC]; static MStat ms[MB];
static uint64_t live_ep = 1, pend_gen = 1, st_ep = 1, snap_epoch = 1, next_gen = 1, next_buf = 0x1000, slot_serial = 100;
static uint64_t pend_lo = 0, pend_n = 0; static bool st_mode = false;
struct MView { int how; uint64_t buf, off; int chunk; };
static uint64_t caddr(int c, uint64_t off) { return 0x100000000ull + uint64_t(c) * CHUNK + off; }
static MView model(uint64_t addr, uint64_t bytes) {
    MView v{0, 0, 0, -1};
    const bool pend = pend_n && addr < pend_lo + pend_n && pend_lo < addr + bytes;
    if (st_mode) {
        for (int i = 0; i < MB; i++) { const MStat &b = ms[i]; if (b.on && !b.demoted && !pend && addr >= b.lo && addr + bytes <= b.lo + b.n) { v.how = 1; v.buf = b.copy_buf; v.off = b.copy_off + (addr - b.lo); return v; } }
        return v;   // (the engine's memo keeps zero-copy views only while static copies are off)
    }
    const int c = int((addr - 0x100000000ull) / CHUNK); const uint64_t off = (addr - 0x100000000ull) % CHUNK;
    if (c < 0 || c >= MC || !mc[c].live || off + bytes > CHUNK || pend) return v;
    for (uint64_t k = off >> 12; k <= (off + bytes - 1) >> 12; k++) { const uint64_t w = mc[c].wr_serial[k]; if (w && w + 8 > slot_serial) return v; }   // recent: the copy path
    v.how = 2; v.buf = mc[c].buf; v.off = off; v.chunk = c; return v;
}
static void z3c_random() {
    for (int i = 0; i < MC; i++) { mc[i] = MChunk{}; mc[i].live = true; mc[i].gen = next_gen++; mc[i].buf = next_buf++; }
    for (int i = 0; i < MB; i++) { ms[i] = MStat{true, false, caddr(i % MC, uint64_t(i) * 2048), 2048, next_buf++, uint64_t(rn(64)) * 16}; }
    nrz_vtab vt; CHECK(nrz_vinit(&vt, 64) == 0); CHECK(nrz_vinit(&vt, 48) != 0); CHECK(nrz_vinit(&vt, 64) == 0);
    long hits = 0, asks = 0;
    static uint64_t noted[96][2];   // the slot serial in which the model last served mesh m in place at each size (the engine notes the serve once a submission: NR-TRX)
    for (long step = 0; step < 2000000; step++) {
        const unsigned op = rn(25) ? rn(880) : rn(1000);
        if (op < 880) {   // a draw asks a range (a small set of meshes, so that asks repeat; two call sites)
            const unsigned m = rn(96); const unsigned alt = rn(8) == 0; const uint64_t addr = caddr(int(m % MC), uint64_t(m) * 640 + (m & 1 ? 16 : 0)), bytes = 256 + ((m + alt) % 5) * 128; const uint32_t site = 1 + (m & 1);
            ++asks; nrz_view *e = nrz_vslot(&vt, addr, bytes); bool fresh = false;
            if (nrz_vmatch(e, addr, bytes, site)) {
                if (e->kind == NRZV_INPLACE) { if (!st_mode && e->chunk >= 0 && e->chunk < MC) { const uint64_t st[4] = {live_ep, mc[e->chunk].wr_ver, slot_serial, pend_gen}; fresh = nrz_vfresh(e, st) != 0; } }
                else if (st_mode) { const uint64_t st[4] = {snap_epoch, st_ep, 0, pend_gen}; fresh = nrz_vfresh(e, st) != 0; }
            }
            const MView r = model(addr, bytes);
            if (fresh) { ++hits; CHECK(r.how == int(e->kind) && r.buf == e->buf && r.off == e->off && e->bytes == bytes);
                if (e->kind == NRZV_INPLACE) CHECK(noted[m][alt] == slot_serial); }   // answered from the table: the serve of this submission was noted by a real ask
            else if (r.how == 2 && !st_mode) noted[m][alt] = slot_serial;
            nrz_view n{}; n.addr = addr; n.bytes = bytes; n.tag = site; n.buf = r.buf; n.off = r.off; n.chunk = -1;
            if (r.how == 1 && st_mode) { n.kind = NRZV_STATIC; n.st[0] = snap_epoch; n.st[1] = st_ep; n.st[3] = pend_gen; *e = n; }
            else if (r.how == 2 && !st_mode) { n.kind = NRZV_INPLACE; n.chunk = r.chunk; n.st[0] = live_ep; n.st[1] = mc[r.chunk].wr_ver; n.st[2] = slot_serial; n.st[3] = pend_gen; *e = n; }
            else if (nrz_vmatch(e, addr, bytes, site)) e->kind = NRZV_NONE;
        }
        else if (op < 900) { MChunk &c = mc[rn(MC)]; c.wr_serial[rn(16)] = slot_serial; if (NRZE_MODEL_DROP != 3) ++c.wr_ver; }          // a replay CPU write (zc::mark)
        else if (op < 915) { MChunk &c = mc[rn(MC)]; if (c.live) { c.live = false; } else { c = MChunk{}; c.live = true; c.gen = next_gen++; c.buf = next_buf++; } if (NRZE_MODEL_DROP != 2) ++live_ep; }   // retired / mapped again (another buffer)
        else if (op < 935) { if (rn(2)) { pend_lo = caddr(int(rn(MC)), rn(CHUNK / 2)); pend_n = 4096; } else pend_n = 0; if (NRZE_MODEL_DROP != 4) ++pend_gen; }
        else if (op < 955) ++slot_serial;                                                                                             // the class of a written range ages with it
        else if (op < 970) { MStat &b = ms[rn(MB)]; b.demoted = !b.demoted; if (!b.demoted) { b.copy_buf = next_buf++; b.copy_off = uint64_t(rn(64)) * 16; } if (NRZE_MODEL_DROP != 5) ++st_ep; }   // demoted / trimmed and copied again elsewhere
        else if (op < 985) { if (rn(4) == 0) for (auto &b : ms) { b.copy_off += 16; } if (NRZE_MODEL_DROP != 6) ++snap_epoch; }      // a new epoch: copies may be made again
        else st_mode = !st_mode;                                                                                                       // (another device class: the kinds never mix in one run of the engine)
    }
    CHECK(hits * 10 > asks);
    {   // an entry answers only its own range and call site, and only under all four words
        nrz_view e{}; e.kind = NRZV_INPLACE; e.addr = 0x5000; e.bytes = 512; e.tag = 1; e.st[0] = 7; e.st[1] = 8; e.st[2] = 9; e.st[3] = 10;
        CHECK(nrz_vmatch(&e, 0x5000, 512, 1)); CHECK(!nrz_vmatch(&e, 0x5010, 512, 1)); CHECK(!nrz_vmatch(&e, 0x5000, 640, 1)); CHECK(!nrz_vmatch(&e, 0x5000, 512, 2));
        for (unsigned k = 0; k < NRZV_STAMPS; k++) { uint64_t st[NRZV_STAMPS] = {7, 8, 9, 10}; CHECK(nrz_vfresh(&e, st)); st[k]++; CHECK(!nrz_vfresh(&e, st)); }
        e.kind = NRZV_NONE; CHECK(!nrz_vmatch(&e, 0x5000, 512, 1)); }
    printf("  Z3c random: %ld asks, the table stood %ld\n", asks, hits);
    nrz_vfree(&vt);
}

// ---------------- Z3d ----------------
static void z3d_random() {
    long served = 0, asked = 0;
    for (int round = 0; round < 4000; round++) {
        const uint64_t chunk_lo = 0x200000000ull + (uint64_t(rn(64)) << 16), chunk_n = (uint64_t(4 + rn(60))) << 12;
        std::vector<uint8_t> sticky(size_t(chunk_n >> 12), 0);
        const uint64_t ring_pages = 1 + rn(unsigned(chunk_n >> 12)), ring_lo = chunk_lo + (uint64_t(rn(unsigned((chunk_n >> 12) - ring_pages + 1))) << 12), ring_hi = ring_lo + (ring_pages << 12) - (rn(3) ? 0 : 256 * rn(8));
        const unsigned nst = rn(4) ? 0 : 1 + rn(3); for (unsigned k = 0; k < nst; k++) sticky[rn(unsigned(sticky.size()))] = 1;
        static const uint64_t als[5] = {1, 16, 64, 256, 4};
        nrz_ring r{}; r.lo = ring_lo; r.hi = ring_hi; r.chunk_lo = chunk_lo; r.align = als[rn(5)];
        uint64_t st_ver = 0, seen_ver = 0;
        r.ok = nrz_ring_clean(sticky.data(), chunk_lo, chunk_n, ring_lo, ring_hi);
        CHECK(!nrz_ring_clean(sticky.data(), chunk_lo, chunk_n, ring_lo, chunk_lo + chunk_n + 1));   // a ring that leaves the chunk is never clean
        CHECK(!nrz_ring_clean(sticky.data(), chunk_lo, chunk_n, chunk_lo - 4096, ring_hi));
        for (int k = 0; k < 400; k++) {
            if (rn(50) == 0) {   // a replay write: a page turns sticky (zc::mark) - the engine makes the word again when the chunk's st_ver moved
                const size_t pg = rn(unsigned(sticky.size())); if (!sticky[pg]) { sticky[pg] = 1; if (NRZE_MODEL_DROP != 7) ++st_ver; }
            }
            if (seen_ver != st_ver) { seen_ver = st_ver; r.ok = nrz_ring_clean(sticky.data(), chunk_lo, chunk_n, ring_lo, ring_hi); }
            const uint64_t addr = ring_lo - 512 + rn(unsigned(ring_hi - ring_lo + 1024)) / (rn(3) ? 16 : 1) * (rn(3) ? 16 : 1), n = 16 + 16 * rn(600);
            uint64_t off = ~0ull, o2 = ~0ull; ++asked;
            if (nrz_ring_off(&r, addr, n, &off)) { ++served;
                CHECK(nb_ubo_view(addr, n, chunk_lo, chunk_n, r.align, sticky.data(), &o2) == NBU_DIRECT && o2 == off);
                CHECK(addr >= ring_lo && addr + n <= ring_hi); }
        }
    }
    CHECK(served * 20 > asked);
    printf("  Z3d random: %ld blocks asked, the ring word answered %ld\n", asked, served);
    // the word itself
    nrz_ring r{}; r.lo = 0x1000; r.hi = 0x3000; r.chunk_lo = 0; r.align = 256; uint64_t off = 0;
    CHECK(!nrz_ring_off(&r, 0x1100, 64, &off));                 // not valid
    r.ok = 1; CHECK(nrz_ring_off(&r, 0x1100, 64, &off) && off == 0x1100);
    CHECK(!nrz_ring_off(&r, 0x1110, 64, &off)); CHECK(!nrz_ring_off(&r, 0x2f00, 0x200, &off)); CHECK(!nrz_ring_off(&r, 0x3000, 16, &off)); CHECK(!nrz_ring_off(&r, 0xf00, 16, &off));
    CHECK(nrz_ring_off(&r, 0x2f00, 0x100, &off) && off == 0x2f00); CHECK(!nrz_ring_off(&r, 0x1100, 0, &off));
}

int main() {
    // the quiet word: never stands before it was set; each of its three words
    { nrz_quiet q{}; CHECK(!nrz_quiet_ok(&q, 0, 0, 0)); nrz_quiet_set(&q, 3, 4, 5); CHECK(nrz_quiet_ok(&q, 3, 4, 5)); CHECK(!nrz_quiet_ok(&q, 4, 4, 5)); CHECK(!nrz_quiet_ok(&q, 3, 5, 5)); CHECK(!nrz_quiet_ok(&q, 3, 4, 6)); }
    z3b_each(); z3b_random(); z3c_random(); z3d_random(); z8i::each(); z8i::random();
    printf("nr_z3e_test: %ld checks, %ld failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
