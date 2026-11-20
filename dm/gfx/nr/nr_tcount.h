// [DOS2] NR-TCOUNT DM_NR_TCOUNT: tiler plan step 3 (docs/port-kb/performance.md PERF-X19 T3 T4 T5; docs/dos2/nr/NR-TCOUNT.md). COUNTING
// ONLY: nothing here changes a pass, a draw or an order. Pure C++ (no Vulkan): the backend (xbg_nr_tcount.inc) fills an nrtc_frame from the
// pass graph of a sampled frame (namespace pgraph, DM_GFX_PASS_GRAPH's recorder) and the host test tests/nr_tcount_test.cpp builds
// synthetic frames; both call the same nrtc_analyze / nrtc_lines.
//
// The frame model (as pgraph records it, present to present):
//   imgs    every image a pass attached / an op or a draw read or wrote: guest serial, NVN format, size, depth
//   passes  each Vulkan render pass begun: attachments (load: kind / clear bits, store bits), draws, reads, storage-image writes,
//           buffer writes, the submit it was recorded in, the host op / line / read break that ended it; + this file's draw facts:
//           VS-only draws (a pre-raster stage writes storage and nothing reaches a colour / depth / stencil attachment: rasterizer discard,
//           or no fragment shader with depth and stencil writes off), VS storage writers that also draw, Iggy UI class
//   ops     transfers / clears / dispatches outside a pass (what they read and write)
//   evs     P pass begun, E pass ended, O op, C query / report (CPU-visible), B buffer write by the command stream (COPY_B2B,
//           UPDATE_UB, COPY_T2B, CLEAR_BUF); a submit is the pass's `sub` number
// Cost model (tiler: every open loads its attachments unless CLEAR / DONT_CARE and stores them unless DONT_CARE): 4 B/px per attachment,
// whatever the format. "A re-open" = the store of the earlier open + the load of the later one.
//
// Questions answered per frame, 3D passes and Iggy UI passes apart (a pass is UI when one of its draws has an Iggy fragment shader):
//   T3  VS-only draws inside passes, the passes holding them, passes made of nothing else (their whole open is the saving), and re-opens of
//       the same attachments next to a VS storage write (the host ends a pass for the storage barrier: before the writer when the tile WAR
//       barrier fires, at the reader's descriptors otherwise)
//   T4  how often each image is opened (1 / 2 / 3+), the MB of all re-opens; for each pass, the next pass with the same attachment set:
//       with other passes between - can one of the two move next to the other (no image read / write dependency, no submit, no
//       query / report between)? exact = no buffer writes involved either (buffers are not tracked by address)
//   T5  the same pair with only copies / clears / dispatches between: removable when each op writes the target (drawable in the pass:
//       vkCmdClearAttachments / a quad) or is independent (moves out); with nothing between: mergeable unless the second pass reads the
//       target (feedback) or a submit lies between; the end reasons of those back-to-back pairs
#ifndef DM_GFX_NR_TCOUNT_H
#define DM_GFX_NR_TCOUNT_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct nrtc_img { uint64_t serial = 0; uint32_t fmt = 0, w = 0, h = 0; bool depth = false; };
struct nrtc_rd { int img = -1; char how = 's'; };   // s sampled, u storage, t transfer source
struct nrtc_pass {
    std::vector<int> att; uint32_t w = 0, h = 0;
    char kind = 'L';            // L load, N don't care (colour, NOLOAD), F load + clear in pass, K host clear pass (pgraph letters)
    uint32_t clear = 0;         // 2 bits per attachment: cleared by the load op
    uint32_t store = 0;         // bit per attachment: DONT_CARE store
    uint32_t dc = 0;            // bit per attachment: DONT_CARE load decided by the NB plan (nbrp_begin: plan, NR-10 fresh attachments)
    uint32_t draws = 0, vso = 0, vsw = 0;   // draws; VS-only draws; VS storage writers that also reach an attachment
    bool first_vso = false, last_vso = false, first_vsw = false, last_vsw = false;   // vsw flags: any VS storage write (VS-only included)
    bool ui = false, buf_write = false, side = false;
    int sub = 0;                // submit number at begin
    std::vector<nrtc_rd> in; std::vector<int> xout;   // reads; storage images written
    unsigned end_op = 0; int end_line = 0, end_img = -1; unsigned end_why = 0; bool ended = true;
    uint32_t fs = 0, vs = 0;    // first 8 hex digits of the first draw's FS key; of the first VS-only draw's VS key
};
struct nrtc_op { char k = 'x'; std::vector<nrtc_rd> in; std::vector<int> out; unsigned host_op = 0; bool buf_write = false; };   // k: x transfer / clear, d dispatch
struct nrtc_ev { char k = 'P'; int i = 0; };
struct nrtc_frame { std::vector<nrtc_img> imgs; std::vector<nrtc_pass> passes; std::vector<nrtc_op> ops; std::vector<nrtc_ev> evs; uint32_t lost_reads = 0; };

enum { NRTC_3D = 0, NRTC_UI = 1 };
struct nrtc_cls {
    uint64_t passes = 0, draws = 0, open_bytes = 0;
    // T3
    uint64_t vso = 0, vsw = 0, vso_passes = 0, vso_only = 0, vso_only_bytes = 0, vsw_split = 0, vso_split = 0, vsw_split_bytes = 0;
    // T4
    uint64_t img_n[3] = {0, 0, 0}, max_open = 0, reopen = 0, reopen_bytes = 0;
    uint64_t aba = 0, aba_bytes = 0, aba_ok = 0, aba_exact = 0, aba_ok_bytes = 0, aba_touch = 0, aba_dep = 0, aba_sub = 0, aba_cpu = 0;
    uint64_t diff = 0, diff_bytes = 0;
    // T5
    uint64_t ops = 0, ops_bytes = 0, ops_ok = 0, ops_ok_bytes = 0, op_into = 0, op_indep = 0, op_out = 0, op_dep = 0, ops_sub = 0;
    uint64_t aa = 0, aa_bytes = 0, aa_ok = 0, aa_ok_bytes = 0, aa_fb = 0, aa_sub = 0;
};
struct nrtc_target {
    uint64_t serial = 0; uint32_t fmt = 0, w = 0, h = 0; bool depth = false, ui = false;
    uint64_t frames = 0, opens = 0, reopen_bytes = 0, pairs_between = 0, pairs_ok = 0;
    std::string between;        // passes between two opens (latest frame that had some)
};
struct nrtc_vs { uint64_t n = 0; std::string where; };
struct nrtc_acc {
    uint64_t frames = 0, frames3d = 0, lost_reads = 0;
    nrtc_cls c[2];
    std::unordered_map<std::string, uint64_t> aa_end, op_kind;
    std::unordered_map<uint64_t, nrtc_target> tg;
    std::unordered_map<uint32_t, nrtc_vs> vs;
};

// ---- names
static inline std::string nrtc_opname(unsigned op) {
    static const char *const hn[40] = {"END", "JUMP", "BEGIN", "CALL", "SIGNAL", "REPORT", "COPY_B2B", "UPDATE_UB", "SET_RT", "CLEAR_COLOR",
        "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE", "BIND_PROGRAM", "BIND_STATE", "VATTR", "VSTREAM", "VBUF", "UBO", "SSBO", "TEX", "SAMP",
        "STENCIL", "POLY_OFFSET", "DRAW", "DRAW_IDX", "DISPATCH", "BARRIER", "TEX_POOL", "SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR",
        "COPY_T2B", "BIND_IMG", "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX", "COPY_IMG_STRIDE"};
    if (op < 40) return hn[op];
    switch (op) {   // the backend static_asserts these against nvn_obj.h; 60 / 61 / 62 are the host's own markers (pgraph)
    case 46: return "RESOLVE"; case 49: return "TYPED_CLEAR"; case 53: return "CLEAR_BUF"; case 55: return "DRAW_TEX";
    case 60: return "submit"; case 61: return "present"; case 62: return "writeback";
    default: return "op" + std::to_string(op);
    }
}
static inline std::string nrtc_label(const nrtc_frame &f, const nrtc_pass &p) {
    char b[64];
    if (p.att.empty()) { snprintf(b, sizeof b, "%ux%u:-:fs%08x", p.w, p.h, p.fs); return b; }
    const nrtc_img &m = f.imgs[size_t(p.att[0])];
    snprintf(b, sizeof b, "%ux%u:%x%s:fs%08x%s", m.w, m.h, m.fmt, m.depth ? "d" : "", p.fs, p.kind == 'K' ? ":clear" : "");
    return b;
}

// ---- cost model
static inline uint64_t nrtc_px4(const nrtc_img &m) { return uint64_t(m.w) * m.h * 4u; }
static inline bool nrtc_loads(const nrtc_frame &f, const nrtc_pass &p, size_t k) {
    if (p.kind == 'K') return false;
    if (p.kind == 'F') return true;
    if (k < 16 && ((p.clear >> (2 * k)) & 3u)) return false;
    if (k < 32 && ((p.dc >> k) & 1u)) return false;
    if (p.kind == 'N' && !f.imgs[size_t(p.att[k])].depth) return false;
    return true;
}
static inline bool nrtc_stores(const nrtc_pass &p, size_t k) { return k >= 32 || !((p.store >> k) & 1u); }
static inline size_t nrtc_slot(const nrtc_pass &p, int img) { for (size_t k = 0; k < p.att.size(); k++) if (p.att[k] == img) return k; return SIZE_MAX; }
static inline uint64_t nrtc_open_bytes(const nrtc_frame &f, const nrtc_pass &p) {
    uint64_t b = 0;
    for (size_t k = 0; k < p.att.size(); k++) { const uint64_t px = nrtc_px4(f.imgs[size_t(p.att[k])]); b += (nrtc_loads(f, p, k) ? px : 0) + (nrtc_stores(p, k) ? px : 0); }
    return b;
}
// the re-open of image x: the earlier open stores it, the later loads it
static inline uint64_t nrtc_reopen_bytes(const nrtc_frame &f, const nrtc_pass &a, const nrtc_pass &b, int x) {
    const uint64_t px = nrtc_px4(f.imgs[size_t(x)]);
    const size_t ka = nrtc_slot(a, x), kb = nrtc_slot(b, x);
    return (ka != SIZE_MAX && nrtc_stores(a, ka) ? px : 0) + (kb != SIZE_MAX && nrtc_loads(f, b, kb) ? px : 0);
}

// ---- the analysis of one frame
static inline uint64_t nrtc_tkey(const nrtc_img &m) {
    uint64_t h = m.serial * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(m.fmt) << 40) ^ (uint64_t(m.w) << 20) ^ m.h ^ (m.depth ? 1ull << 63 : 0);
    return h;
}
static inline void nrtc_analyze(const nrtc_frame &f, nrtc_acc &a) {
    const size_t np = f.passes.size(), ni = f.imgs.size();
    a.frames++; a.lost_reads += f.lost_reads;
    std::vector<int> pos(np, -1), epos(np, -1);
    for (size_t e = 0; e < f.evs.size(); e++) {
        const nrtc_ev &v = f.evs[e];
        if (v.i < 0) continue;
        if (v.k == 'P' && size_t(v.i) < np) pos[size_t(v.i)] = int(e);
        else if (v.k == 'E' && size_t(v.i) < np) epos[size_t(v.i)] = int(e);
    }
    for (size_t i = 0; i < np; i++) if (epos[i] < 0) epos[i] = pos[i];
    bool has3d = false;
    std::vector<std::vector<int>> sorted(np);
    for (size_t i = 0; i < np; i++) {
        const nrtc_pass &p = f.passes[i]; nrtc_cls &c = a.c[p.ui ? NRTC_UI : NRTC_3D];
        if (!p.ui && p.draws) has3d = true;
        c.passes++; c.draws += p.draws;
        const uint64_t ob = nrtc_open_bytes(f, p); c.open_bytes += ob;
        c.vso += p.vso; c.vsw += p.vsw;
        if (p.vso) {
            c.vso_passes++;
            if (p.vso == p.draws) { c.vso_only++; c.vso_only_bytes += ob; }
            nrtc_vs &v = a.vs[p.vs]; v.n += p.vso; if (v.where.empty()) v.where = nrtc_label(f, p);
        }
        sorted[i] = p.att; std::sort(sorted[i].begin(), sorted[i].end());
        sorted[i].erase(std::unique(sorted[i].begin(), sorted[i].end()), sorted[i].end());
    }
    if (has3d) a.frames3d++;
    // ---- per image: opens in event order (K clear passes included: they open the target too)
    std::vector<std::vector<int>> opens(ni);
    for (size_t e = 0; e < f.evs.size(); e++) {
        const nrtc_ev &v = f.evs[e];
        if (v.k != 'P' || v.i < 0 || size_t(v.i) >= np) continue;
        for (int x : sorted[size_t(v.i)]) if (x >= 0 && size_t(x) < ni) opens[size_t(x)].push_back(v.i);
    }
    auto lbl_between = [&](int i, int j) {
        std::string s; unsigned n = 0, more = 0;
        for (int e = epos[size_t(i)] + 1; e < pos[size_t(j)]; e++) {
            const nrtc_ev &v = f.evs[size_t(e)];
            if (v.k != 'P') continue;
            const nrtc_pass &k = f.passes[size_t(v.i)];
            if (k.kind == 'K') continue;
            const std::string l = nrtc_label(f, k);
            if (s.find(l) != std::string::npos) continue;
            if (n < 3) { s += (n ? "," : "") + l; n++; } else more++;
        }
        if (more) s += ",+" + std::to_string(more);
        return s;
    };
    for (size_t x = 0; x < ni; x++) {
        const std::vector<int> &o = opens[x];
        if (o.empty()) continue;
        bool ui = false; for (int p : o) ui |= f.passes[size_t(p)].ui;
        nrtc_cls &c = a.c[ui ? NRTC_UI : NRTC_3D];
        c.img_n[o.size() >= 3 ? 2 : o.size() - 1]++;
        if (o.size() > c.max_open) c.max_open = o.size();
        uint64_t rb = 0;
        for (size_t k = 1; k < o.size(); k++) rb += nrtc_reopen_bytes(f, f.passes[size_t(o[k - 1])], f.passes[size_t(o[k])], int(x));
        c.reopen += o.size() - 1; c.reopen_bytes += rb;
        // consecutive real (non-K) opens with another attachment set / size: subpass territory, not a reorder
        nrtc_target &t = a.tg[nrtc_tkey(f.imgs[x])];
        if (!t.frames) { t.serial = f.imgs[x].serial; t.fmt = f.imgs[x].fmt; t.w = f.imgs[x].w; t.h = f.imgs[x].h; t.depth = f.imgs[x].depth; }
        t.ui |= ui; t.frames++; t.opens += o.size(); t.reopen_bytes += rb;
        int prev = -1; std::string betw;
        for (int p : o) {
            if (f.passes[size_t(p)].kind == 'K') continue;
            if (prev >= 0) {
                const nrtc_pass &pi = f.passes[size_t(prev)], &pj = f.passes[size_t(p)];
                if (sorted[size_t(prev)] != sorted[size_t(p)] || pi.w != pj.w || pi.h != pj.h) {
                    nrtc_cls &d = a.c[(pi.ui || pj.ui) ? NRTC_UI : NRTC_3D]; d.diff++;
                    // the bytes of x from prev to p (K opens between included)
                    uint64_t b = 0; size_t s = 0; while (o[s] != prev) s++;
                    for (size_t k = s + 1; k < o.size(); k++) { b += nrtc_reopen_bytes(f, f.passes[size_t(o[k - 1])], f.passes[size_t(o[k])], int(x)); if (o[k] == p) break; }
                    d.diff_bytes += b;
                }
                if (betw.empty()) betw = lbl_between(prev, p);
            }
            prev = p;
        }
        if (!betw.empty()) t.between = betw;
    }
    // ---- pass pairs: i and the next real pass j with the same attachment set and size
    std::vector<char> inA(ni, 0), inWi(ni, 0), inRi(ni, 0), inWj(ni, 0), inRj(ni, 0);
    auto any = [&](const std::vector<char> &m, const std::vector<int> &v) { for (int x : v) if (x >= 0 && size_t(x) < ni && m[size_t(x)]) return true; return false; };
    auto anyr = [&](const std::vector<char> &m, const std::vector<nrtc_rd> &v) { for (const nrtc_rd &r : v) if (r.img >= 0 && size_t(r.img) < ni && m[size_t(r.img)]) return true; return false; };
    auto set = [&](std::vector<char> &m, const std::vector<int> &v, char on) { for (int x : v) if (x >= 0 && size_t(x) < ni) m[size_t(x)] = on; };
    auto setr = [&](std::vector<char> &m, const std::vector<nrtc_rd> &v, char on) { for (const nrtc_rd &r : v) if (r.img >= 0 && size_t(r.img) < ni) m[size_t(r.img)] = on; };
    for (size_t i = 0; i < np; i++) {
        const nrtc_pass &pi = f.passes[i];
        if (pi.kind == 'K' || pi.att.empty()) continue;
        size_t j = i + 1;
        for (; j < np; j++) { const nrtc_pass &q = f.passes[j]; if (q.kind != 'K' && sorted[j] == sorted[i] && q.w == pi.w && q.h == pi.h) break; }
        if (j >= np) continue;
        const nrtc_pass &pj = f.passes[j];
        nrtc_cls &c = a.c[(pi.ui || pj.ui) ? NRTC_UI : NRTC_3D];
        uint64_t bytes = 0; for (int x : sorted[i]) bytes += nrtc_reopen_bytes(f, pi, pj, x);
        set(inA, pi.att, 1); set(inWi, pi.att, 1); set(inWi, pi.xout, 1); setr(inRi, pi.in, 1);
        set(inWj, pj.att, 1); set(inWj, pj.xout, 1); setr(inRj, pj.in, 1);
        const bool sub = pj.sub != pi.sub;
        bool real = false, items = false, cpu = false, buf = pi.buf_write || pj.buf_write, touch = false, up_bad = false, down_bad = false;
        unsigned into = 0, indep = 0, outr = 0, dep = 0; uint64_t kbytes = 0;
        std::vector<std::string> names;
        for (int e = epos[i] + 1; e < pos[j]; e++) {
            const nrtc_ev &v = f.evs[size_t(e)];
            if (v.k == 'C') { cpu = true; continue; }
            if (v.k == 'B') { buf = true; continue; }
            if (v.k != 'P' && v.k != 'O') continue;
            const std::vector<int> *W1, *W2 = nullptr; const std::vector<nrtc_rd> *R; bool kb = false, side = false; std::string nm;
            if (v.k == 'P') {
                const nrtc_pass &k = f.passes[size_t(v.i)];
                W1 = &k.att; W2 = &k.xout; R = &k.in; kb = k.buf_write; side = k.side;
                if (k.kind == 'K') { nm = "clear pass"; kbytes += nrtc_open_bytes(f, k); } else real = true;
            } else {
                const nrtc_op &k = f.ops[size_t(v.i)];
                W1 = &k.out; R = &k.in; kb = k.buf_write; nm = k.k == 'd' ? "dispatch" : nrtc_opname(k.host_op);
            }
            items = true; if (kb) buf = true; if (side) cpu = true;
            const bool wA = any(inA, *W1) || (W2 && any(inA, *W2)), rA = anyr(inA, *R);
            if (wA || rA) touch = true;
            if (any(inRj, *W1) || any(inWj, *W1) || (W2 && (any(inRj, *W2) || any(inWj, *W2))) || anyr(inWj, *R)) up_bad = true;
            if (anyr(inWi, *R) || any(inRi, *W1) || any(inWi, *W1) || (W2 && (any(inRi, *W2) || any(inWi, *W2)))) down_bad = true;
            if (!real && !nm.empty()) {   // T5: what the op is to the target
                const char *cls;
                if (rA) { outr++; cls = "reads the target"; }
                else if (wA) { into++; cls = "into the target"; }
                else {
                    const bool before = !anyr(inWi, *R) && !any(inRi, *W1) && !any(inWi, *W1) && !(W2 && (any(inRi, *W2) || any(inWi, *W2)));
                    const bool after = !any(inRj, *W1) && !any(inWj, *W1) && !(W2 && (any(inRj, *W2) || any(inWj, *W2))) && !anyr(inWj, *R);
                    if ((before || after) && !kb) { indep++; cls = "independent"; } else { dep++; cls = "dependent"; }
                }
                names.push_back(nm + " " + cls);
            }
        }
        if (!items) {   // back to back
            c.aa++; c.aa_bytes += bytes;
            const bool fb = anyr(inA, pj.in);
            if (fb) c.aa_fb++; else if (sub) c.aa_sub++; else if (!cpu) { c.aa_ok++; c.aa_ok_bytes += bytes; }
            std::string why;
            if (pi.end_img >= 0) why = "read break why=" + std::to_string(pi.end_why);
            else if (!pi.end_line) why = "host";
            else why = nrtc_opname(pi.end_op) + "/L" + std::to_string(pi.end_line);
            a.aa_end[why]++;
        } else if (!real) {   // only copies / clears / dispatches between
            const uint64_t b = bytes + kbytes;
            c.ops++; c.ops_bytes += b; c.op_into += into; c.op_indep += indep; c.op_out += outr; c.op_dep += dep;
            if (sub) c.ops_sub++;
            else if (!outr && !dep && !cpu) { c.ops_ok++; c.ops_ok_bytes += b; }
            for (const std::string &n : names) a.op_kind[n]++;
        } else {   // other passes between: reorder?
            c.aba++; c.aba_bytes += bytes;
            const bool ok = !(up_bad && down_bad) && !sub && !cpu;
            if (ok) { c.aba_ok++; c.aba_ok_bytes += bytes; if (!buf) c.aba_exact++; }
            else if (sub) c.aba_sub++;
            else if (cpu) c.aba_cpu++;
            else if (touch) c.aba_touch++;
            else c.aba_dep++;
            for (int x : sorted[i]) { nrtc_target &t = a.tg[nrtc_tkey(f.imgs[size_t(x)])]; t.pairs_between++; if (ok) t.pairs_ok++; }
        }
        if (!real && (pi.last_vsw || pj.first_vsw)) { c.vsw_split++; c.vsw_split_bytes += bytes; if (pi.last_vso || pj.first_vso) c.vso_split++; }
        set(inA, pi.att, 0); set(inWi, pi.att, 0); set(inWi, pi.xout, 0); setr(inRi, pi.in, 0);
        set(inWj, pj.att, 0); set(inWj, pj.xout, 0); setr(inRj, pj.in, 0);
    }
}

// ---- the lines (each < 1024 B: xbg_logf's buffer)
static inline std::string nrtc_top(const std::unordered_map<std::string, uint64_t> &m, unsigned n, double f) {
    std::vector<std::pair<std::string, uint64_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const std::pair<std::string, uint64_t> &x, const std::pair<std::string, uint64_t> &y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
    std::string s; char b[160];
    for (size_t k = 0; k < v.size() && k < n; k++) { snprintf(b, sizeof b, "%s%s %.2f", k ? ", " : "", v[k].first.c_str(), double(v[k].second) / f); s += b; }
    return s.empty() ? "none" : s;
}
static inline void nrtc_lines(const nrtc_acc &a, int mode, unsigned every, std::string out[4]) {
    const double f = a.frames ? double(a.frames) : 1., MB = 1048576.;
    const nrtc_cls &d = a.c[NRTC_3D], &u = a.c[NRTC_UI];
    char b[1024];
    // T3
    std::vector<std::pair<uint32_t, nrtc_vs>> vs(a.vs.begin(), a.vs.end());
    std::sort(vs.begin(), vs.end(), [](const std::pair<uint32_t, nrtc_vs> &x, const std::pair<uint32_t, nrtc_vs> &y) { return x.second.n != y.second.n ? x.second.n > y.second.n : x.first < y.first; });
    std::string vl;
    for (size_t k = 0; k < vs.size() && k < 3; k++) { char t[128]; snprintf(t, sizeof t, "%svs%08x %.2f in %s", k ? ", " : "", vs[k].first, double(vs[k].second.n) / f, vs[k].second.where.c_str()); vl += t; }
    snprintf(b, sizeof b, "[nr] tcount T3 DM_NR_TCOUNT=%d: %llu frames sampled (1 in %u), %llu with 3D; a frame 3D / UI: passes %.1f / %.1f, draws %.0f / %.0f, "
        "load+store %.1f / %.1f MB (4 B/px); VS-only draws %.2f / %.2f in passes %.2f / %.2f (only VS-only %.2f / %.2f = %.1f / %.1f MB), "
        "VS storage writers that also draw %.2f / %.2f; same target re-begun next to a VS storage write %.2f / %.2f (next to a VS-only draw %.2f / %.2f) = %.1f / %.1f MB; "
        "VS-only by shader: %s",
        mode, (unsigned long long)a.frames, every, (unsigned long long)a.frames3d, d.passes / f, u.passes / f, d.draws / f, u.draws / f,
        d.open_bytes / f / MB, u.open_bytes / f / MB, d.vso / f, u.vso / f, d.vso_passes / f, u.vso_passes / f, d.vso_only / f, u.vso_only / f,
        d.vso_only_bytes / f / MB, u.vso_only_bytes / f / MB, d.vsw / f, u.vsw / f, d.vsw_split / f, u.vsw_split / f, d.vso_split / f, u.vso_split / f,
        d.vsw_split_bytes / f / MB, u.vsw_split_bytes / f / MB, vl.empty() ? "none" : vl.c_str());
    out[0] = b;
    // T4
    snprintf(b, sizeof b, "[nr] tcount T4: a frame 3D / UI: images opened 1x %.1f / %.1f, 2x %.1f / %.1f, 3+x %.1f / %.1f (most %llu / %llu); re-opens %.1f / %.1f = %.1f / %.1f MB; "
        "the same attachments again after other passes %.2f / %.2f = %.1f / %.1f MB: reorderable %.2f / %.2f (exact %.2f / %.2f; the rest if buffer writes allow) = %.1f / %.1f MB, "
        "blocked: a pass between uses the target %.2f / %.2f, other image dependency %.2f / %.2f, submit %.2f / %.2f, query / report %.2f / %.2f; "
        "re-opens with another attachment set %.2f / %.2f = %.1f / %.1f MB; reads over the recorder cap %.2f",
        d.img_n[0] / f, u.img_n[0] / f, d.img_n[1] / f, u.img_n[1] / f, d.img_n[2] / f, u.img_n[2] / f, (unsigned long long)d.max_open, (unsigned long long)u.max_open,
        d.reopen / f, u.reopen / f, d.reopen_bytes / f / MB, u.reopen_bytes / f / MB, d.aba / f, u.aba / f, d.aba_bytes / f / MB, u.aba_bytes / f / MB,
        d.aba_ok / f, u.aba_ok / f, d.aba_exact / f, u.aba_exact / f, d.aba_ok_bytes / f / MB, u.aba_ok_bytes / f / MB, d.aba_touch / f, u.aba_touch / f,
        d.aba_dep / f, u.aba_dep / f, d.aba_sub / f, u.aba_sub / f, d.aba_cpu / f, u.aba_cpu / f, d.diff / f, u.diff / f, d.diff_bytes / f / MB, u.diff_bytes / f / MB,
        a.lost_reads / f);
    out[1] = b;
    // T5
    snprintf(b, sizeof b, "[nr] tcount T5: a frame 3D / UI: same target re-begun with only copies / clears / dispatches between %.2f / %.2f = %.1f / %.1f MB: removable %.2f / %.2f = %.1f / %.1f MB "
        "(ops into the target = drawable in the pass %.2f / %.2f, independent = move out %.2f / %.2f, read the target %.2f / %.2f, dependent %.2f / %.2f; across a submit %.2f / %.2f) [%s]; "
        "back-to-back passes into the same target %.2f / %.2f = %.1f / %.1f MB: mergeable %.2f / %.2f = %.1f / %.1f MB, the second reads the target %.2f / %.2f, across a submit %.2f / %.2f; "
        "ended by: %s",
        d.ops / f, u.ops / f, d.ops_bytes / f / MB, u.ops_bytes / f / MB, d.ops_ok / f, u.ops_ok / f, d.ops_ok_bytes / f / MB, u.ops_ok_bytes / f / MB,
        d.op_into / f, u.op_into / f, d.op_indep / f, u.op_indep / f, d.op_out / f, u.op_out / f, d.op_dep / f, u.op_dep / f, d.ops_sub / f, u.ops_sub / f,
        nrtc_top(a.op_kind, 4, f).c_str(), d.aa / f, u.aa / f, d.aa_bytes / f / MB, u.aa_bytes / f / MB, d.aa_ok / f, u.aa_ok / f, d.aa_ok_bytes / f / MB,
        u.aa_ok_bytes / f / MB, d.aa_fb / f, u.aa_fb / f, d.aa_sub / f, u.aa_sub / f, nrtc_top(a.aa_end, 4, f).c_str());
    out[2] = b;
    // top 5 targets by re-open MB
    std::vector<const nrtc_target *> t;
    for (const auto &e : a.tg) if (e.second.reopen_bytes) t.push_back(&e.second);
    std::sort(t.begin(), t.end(), [](const nrtc_target *x, const nrtc_target *y) { return x->reopen_bytes != y->reopen_bytes ? x->reopen_bytes > y->reopen_bytes : x->serial < y->serial; });
    std::string s = "[nr] tcount top: re-opened targets by MB a frame:";
    for (size_t k = 0; k < t.size() && k < 5; k++) {
        char e[320];
        snprintf(e, sizeof e, " %zu) #%llu %ux%u fmt %#x%s%s opened %.1f, re-opens %.1f MB, pairs with passes between %.2f (reorderable %.2f), between: %s;", k + 1,
            (unsigned long long)t[k]->serial, t[k]->w, t[k]->h, t[k]->fmt, t[k]->depth ? " depth" : "", t[k]->ui ? " UI" : "", t[k]->opens / f, t[k]->reopen_bytes / f / MB,
            t[k]->pairs_between / f, t[k]->pairs_ok / f, t[k]->between.empty() ? "-" : t[k]->between.c_str());
        if (s.size() + strlen(e) > 1000) break;
        s += e;
    }
    if (t.empty()) s += " none";
    out[3] = s;
}
#endif
