// [DOS2] NR-ZERO Z8 host test (docs/dos2/nr/NR-ZERO.md section 25): the thread that submits knows each engine draw exactly.
// dm/gfx/nr/nr_z8.h (fold, walk, fix-id dictionaries, templates), nr_z5r.h (the run protocol), nbmtq::Queue and the real wrappers of
// xbg_nb_mt.inc against the mock Vulkan of nb_mt_test.cpp (a "command buffer" is a byte log of the calls made on it). Two real threads.
//
// The world writes REAL command memory, frame after frame: plain state records (render targets, viewport, scissor, depth range,
// state objects, stencil, polygon offset, vertex attributes / streams / buffers, pools, blend colour, alpha reference, ...),
// OP_X_NB_STATE blocks (DEF / REF with epochs, a REF without its DEF), OP_X_NR_STATE packets, OP_X_NR_DRAW records with their binds
// and tail, OP_JUMP between chunks of the memory (with a poison record behind every jump), OP_CALL into a handle with state records
// and a draw, passes and other commands.
//   the translation thread's model   executes the records with its OWN traversal (as execute(): recursion on CALL / state blocks,
//            its own DEF dictionary) into its State, and resolves a draw from that State alone: an "NR-08 entry" from the programs,
//            the viewport size, the pools, the texture / sampler handles of the rows its plan names and the uniform sizes; an
//            "NR-09 entry" from the key id and every state field a pipeline reads (listed here field by field, not through
//            nr_z8.h's groups); the values from the two, its generations, the uniform addresses, the vertex ranges.
//   TODAY    that model alone, direct calls: the reference logs.
//   FOLD     after every frame the walk's copy is the model's State, field for field, and at every draw record the walk's digest
//            is the digest of the model's State (bit for bit).
//   APPLY    (DM_NR_Z8=1) the submitting thread walks the frame first (nr_z8.h Walker + the handler of xbg_nr_z8.inc, repeated
//            here), the translation thread takes the walk's answer where the template version is still its own and its
//            generations stand - WITHOUT resolving from State - computes the values from that answer and asks Reader::draw. The logs
//            must be byte-identical to TODAY's in queue modes 1 / 2 / 3 and several queue shapes; every spare chunk comes back.
//   VERIFY   (DM_NR_Z8=2) everything resolved and recorded by the model; the walk's answer against the model's, the packets of
//            every draw =1 would take against the model's own: bad 0, logs TODAY's.
//   TABLE    one writer and two readers on one slot: no torn template.
// The mutation check: built with -DNRZ8_MUTATE=k (k = 1..14 but 9, nr_z8.h) this test must FAIL (9 - no vertex-range compare in match() -
// must PASS: those words only give values, which the reader compares again).
// Build (Vulkan headers: any vulkan/ + vk_video/ include tree, e.g. the NDK sysroot's usr/include), from the repository root:
//   clang++ -std=c++17 -O2 -Wall -Wextra -DDM_GAME_DOS2 -Idm/gfx/nvn/include -I<vk> dm/gfx/vulkan/engine/tests/nr_z8_test.cpp -o nr_z8_test && ./nr_z8_test
//   g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -DDM_GAME_DOS2 -Idm/gfx/nvn/include -I<vk> dm/gfx/vulkan/engine/tests/nr_z8_test.cpp -o nr_z8_tsan && ./nr_z8_tsan
#include <vulkan/vulkan_core.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::vector<std::string> g_lines;
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void xbg_logf(const char *fmt, ...) { char t[2048]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof t, fmt, a); va_end(a); g_lines.push_back(t); }
#include "nb_mt_mock.h"
#define NBMT_REAL(f) mock_##f
static void (*g_pre)() = nullptr;   // the engine's NBMT_PRE: before a wrapper writes anything (and at nbmt::end / sync)
#define NBMT_PRE(c, op) do { (void)(c); (void)(op); if (g_pre) g_pre(); } while (0)
#include "../xbg_nb_mt.inc"
#if !defined(__clang__)
#define _Static_assert static_assert   // (nvn_obj.h is a C header; g++ has no _Static_assert in C++)
#endif
extern "C" {
#include "../../../nvn/nvn_obj.h"
}
#include "../../../nr/nr_z5r.h"
#include "../../../nr/nr_z8.h"

static long fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
struct Rng { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return uint32_t(s >> 11); } uint32_t upto(uint32_t n) { return n ? next() % n : 0; } };
template <class H> static H hnd(uint64_t x) { return H(uintptr_t(x)); }
static const uint32_t nvn_stage[6] = {0, 3, 4, 2, 1, 5};
static int vattr_replace(uint32_t st[16], const uint32_t *a, int n) { memset(st, 0, 64); memcpy(st, a, size_t(n) * 4); return 0; }

// ---------------- State (the fields of xbg_game.cpp State, same names) ----------------
struct MS {
    p_set_rt rt{}; void *shader[6]{}; NVNbufferRange ub[6][16]{}, sb[6][16]{}, vb[16]{};
    uint64_t tex[6][128]{}, samp[6][128]{}, img[6][16]{}; dpool_t texture_pool{}, sampler_pool{};
    uint32_t attrib[16]{}; vs_st stream[16]{}; blend_st blend[8]{}; color_st color{}; chmask_st mask{0xffffffff}; ms_st ms{}; poly_st poly{0, 1, 2, 0}; ds_st ds{};
    VkViewport viewport{0, 0, 1, 1, 0, 1}; VkRect2D scissor{{0, 0}, {UINT32_MAX, UINT32_MAX}}; float blend_color[4]{}, bias[3]{};
    uint32_t stencil_compare[2]{~0u, ~0u}, stencil_write[2]{~0u, ~0u}, stencil_ref[2]{}; int64_t copy_stride = 0, copy_image_stride = 0; uint32_t patch_size = 3;
    xbg_depth_bounds bounds{0, 0, 1}; p_prim_restart restart{0, 0}; uint32_t depth_clamp = 0, rast_discard = 0, restart_eff = 0; float alpha_ref = 0;
};
struct Sh : MS { uint64_t sprog[6]{}; };                 // the walk's copy
struct TS : MS { const prog_t *tprog[6]{}; };            // the model's State: the program object of each stage
static prog_t g_prog[12]; static dpool_t g_pool[4]; static pool_t g_mem[4];
static bool bind_sh(Sh &S, const prog_t *prog, int32_t stages) { if (prog && prog->nstage > 6) return false; const uint64_t w = prog ? prog->serial : 0; for (unsigned st = 0; st < 6; st++) if (stages & (1u << nvn_stage[st])) S.sprog[st] = w; return true; }
static bool bind_ts(TS &S, const prog_t *prog, int32_t stages) { if (prog && prog->nstage > 6) return false; for (unsigned st = 0; st < 6; st++) if (stages & (1u << nvn_stage[st])) S.tprog[st] = prog; return true; }
#define T_BIND_SH(S, pr, st) do { if (!bind_sh(*(S), reinterpret_cast<const prog_t *>(uintptr_t(pr)), (st))) return -1; } while (0)
#define T_BIND_TS(S, pr, st) do { if (!bind_ts(*(S), reinterpret_cast<const prog_t *>(uintptr_t(pr)), (st))) return -1; } while (0)
#define T_VATTR(a, st, n) vattr_replace((a), (st), (n))
NR3_DEFINE_APPLY_RAW(apply_sh, Sh, T_BIND_SH, T_VATTR)
NR3_DEFINE_APPLY_RAW(apply_ts, TS, T_BIND_TS, T_VATTR)
// field for field (the fold must leave the same bytes in every field it can write)
static const char *state_diff(const MS &a, const MS &b) {
#define F(f) if (memcmp(&a.f, &b.f, sizeof a.f)) return #f;
    F(rt) F(ub) F(sb) F(vb) F(tex) F(samp) F(img) F(attrib) F(stream) F(blend) F(color) F(mask) F(ms) F(poly) F(ds) F(viewport) F(scissor) F(blend_color) F(bias) F(stencil_compare) F(stencil_write)
    F(stencil_ref) F(copy_stride) F(copy_image_stride) F(patch_size) F(bounds) F(restart) F(depth_clamp) F(rast_discard) F(alpha_ref)
#undef F
    if (a.texture_pool.pool != b.texture_pool.pool || a.texture_pool.off != b.texture_pool.off || a.texture_pool.n != b.texture_pool.n) return "texture_pool";
    if (a.sampler_pool.pool != b.sampler_pool.pool || a.sampler_pool.off != b.sampler_pool.off || a.sampler_pool.n != b.sampler_pool.n) return "sampler_pool";
    return nullptr;
}
static uint64_t digest(const MS &s) {   // every field above, in one number
    uint8_t buf[nrz8::GROUP_MAX]; const uint64_t z[6] = {}; uint64_t h = 0;
    for (int g = 0; g < nrz8::GROUPS; g++) h = h * 31 + nrz8::hash_bytes(buf, nrz8::group_bytes(g, s, z, buf));
    h = h * 31 + nrz8::hash_bytes(s.attrib, sizeof s.attrib); h = h * 31 + nrz8::hash_bytes(s.stream, sizeof s.stream);
    h = h * 31 + nrz8::hash_bytes(s.vb, sizeof s.vb); h = h * 31 + nrz8::hash_bytes(s.ub, sizeof s.ub); h = h * 31 + nrz8::hash_bytes(s.tex, sizeof s.tex); h = h * 31 + nrz8::hash_bytes(s.samp, sizeof s.samp);
    return h * 31 + uint64_t(s.copy_stride) * 5 + uint64_t(s.copy_image_stride) * 7;
}

// ---------------- command memory ----------------
enum { OP_T_OTHER = OP_CLEAR_COLOR, OP_T_PASS = OP_X_NB_PASS, OP_T_SILENT = OP_BARRIER };   // records the fold does not know: a command, a pass change, a bind-cache reset without a command
struct Mem {
    std::vector<std::unique_ptr<uint64_t[]>> chunks; uint8_t *p = nullptr, *end = nullptr; Rng *r = nullptr;
    void chunk() { const uint32_t n = 1024 + 512 * r->upto(12); chunks.emplace_back(new uint64_t[n / 8 + 16]()); p = reinterpret_cast<uint8_t *>(chunks.back().get()); end = p + n; }
    static void hdr(uint8_t *q, uint32_t op, uint32_t size) { const op_hdr h = {op, size}; memcpy(q, &h, 8); }
    uint8_t *begin() { chunk(); return p; }
    uint8_t *rec(uint32_t op, uint32_t payload) {
        const uint32_t size = (8 + payload + 7) & ~7u;
        if (p + size + 64 > end) {   // OP_JUMP to a new chunk; behind it a record that must never be read, then an end
            uint8_t *at = p; chunk(); hdr(at, OP_JUMP, 16); const uint64_t t = uint64_t(uintptr_t(p)); memcpy(at + 8, &t, 8);
            hdr(at + 16, OP_POLY_OFFSET, 24); const p_poly_offset po = {77.f, 78.f, 79.f}; memcpy(at + 24, &po, sizeof po); hdr(at + 40, OP_END, 8); }
        uint8_t *q = p; hdr(q, op, size); memset(q + 8, 0, size - 8); p += size; return q + 8;
    }
    template <class T> void put(uint32_t op, const T &v) { memcpy(rec(op, sizeof v), &v, sizeof v); }
    void finish() { hdr(p, OP_END, 8); p += 8; }
};

// ---------------- the world ----------------
struct Mat { uint32_t key; int vp, fp; uint64_t dsb; uint32_t poly, color; uint64_t blend0; uint32_t attr[3]; uint32_t stride0, stride1; };
struct Mesh { uint64_t vb0, vb0s, vb1, vb1s, ibuf; int32_t count, base_vertex, first; bool indexed; int32_t itype; };
struct Gens { uint32_t set_gen, pipe_gen; uint64_t ring_delta; };
struct Frame { std::vector<uint64_t> handles; Gens g; };
struct World {
    Rng r; Rng q{1};   // q: the shape of a draw and its state - the same at the same place of every frame (now and then not)
    Mem mem; std::vector<Mat> mats; std::vector<Mesh> meshes; std::vector<uint32_t> order; Gens g{1, 1, 0x7300000000ull - 4096}; uint32_t head = 0, epoch = 0; uint64_t ring = 0x7300000000ull;
    uint16_t def_epoch[NBS_KINDS][8] = {};
    explicit World(uint64_t seed) : r{seed * 0x9E3779B97F4A7C15ull + 11} {
        mem.r = &r;
        mats.resize(24); for (auto &m : mats) { m.key = 1 + r.upto(40); m.vp = int(r.upto(6)); m.fp = 6 + int(r.upto(6)); m.dsb = r.upto(4) * 0x0101010101ull; m.poly = r.upto(3); m.color = r.upto(3) << 8; m.blend0 = uint64_t(r.upto(3)) << 3;
            for (auto &a : m.attr) a = r.upto(4) << 8; m.stride0 = 16 + 4 * r.upto(3); m.stride1 = 8 + 4 * r.upto(2); }
        meshes.resize(40); for (auto &m : meshes) { m.vb0 = 0x7200000000ull + 4096 * r.upto(64); m.vb0s = 4096 * (1 + r.upto(2)); m.vb1 = 0x7280000000ull + 4096 * r.upto(8); m.vb1s = 4096; m.indexed = r.upto(6) != 0;
            m.itype = 1 + int32_t(r.upto(2)); m.ibuf = 0x7100000000ull + 4096 * r.upto(64); m.count = int32_t(3 * (1 + r.upto(500))); m.base_vertex = int32_t(r.upto(50)); m.first = int32_t(r.upto(90)); }
        for (uint32_t i = 0; i < 260; i++) order.push_back(r.upto(4) ? (i * 7) % 240 : r.upto(240));   // (material, mesh) pairs in a stable order
    }
    // state of a material: as an OP_X_NR_STATE packet, as plain records, or as OP_X_NB_STATE blocks
    void material(const Mat &m, const Mesh &me) {
        const uint32_t how = q.upto(10); const uint64_t alt = r.upto(12) ? 0 : 0x80000000ull;   // vertex stream 1 from another range now and then (the site words are the same)
        if (how < 6) {
            nr3_rec z; memset(&z, 0, sizeof z); z.nprog = 1; z.prog[0] = uint64_t(uintptr_t(&g_prog[m.vp])); z.pstage[0] = 1;
            if (q.upto(2)) { z.nprog = 2; z.prog[1] = uint64_t(uintptr_t(&g_prog[m.fp])); z.pstage[1] = 2; } else { p_bind_program b{&g_prog[m.fp], 2}; mem.put(OP_BIND_PROGRAM, b); }
            z.have = NR3_H_DS | NR3_H_POLY | NR3_H_COLOR | NR3_H_VATTR | NR3_H_VSTREAM; z.ds = m.dsb; z.poly = m.poly; z.color = m.color; z.blend = 1; z.blendb[0] = m.blend0;
            z.nvattr = 3; memcpy(z.vattr, m.attr, sizeof m.attr); z.nstream = 2; z.vstream[0] = m.stride0; z.vstream[1] = m.stride1;
            if (q.upto(3) == 0) { z.have |= NR3_H_SREF; z.sten[2][0] = 1 + int32_t(q.upto(3)); z.sten[2][1] = int32_t(q.upto(4)); }
            if (q.upto(3) == 0) { z.have |= NR3_H_ALPHA; z.alpha = float(q.upto(3)) * 0.25f; }
            z.vb = 3; z.vba[0] = me.vb0; z.vbs[0] = me.vb0s; z.vba[1] = me.vb1 + alt; z.vbs[1] = me.vb1s;
            nr3_put(&z, mem.rec(OP_X_NR_STATE, nr3_size(&z)));
        } else {
            p_bind_program b{&g_prog[m.vp], 1}; mem.put(OP_BIND_PROGRAM, b); b = p_bind_program{&g_prog[m.fp], 2}; mem.put(OP_BIND_PROGRAM, b);
            p_bind_state s{}; s.kind = 4; memcpy(s.bytes, &m.poly, 4); mem.put(OP_BIND_STATE, s); s = p_bind_state{}; s.kind = 1; memcpy(s.bytes, &m.color, 4); mem.put(OP_BIND_STATE, s);
            s = p_bind_state{}; s.kind = 0; memcpy(s.bytes, &m.blend0, 8); mem.put(OP_BIND_STATE, s);
            if (how < 8) { s = p_bind_state{}; s.kind = 5; memcpy(s.bytes, &m.dsb, 8); mem.put(OP_BIND_STATE, s); }
            else {   // the depth-stencil state as a state block: DEF the first time in this epoch, REF after
                const unsigned id = unsigned(m.dsb >> 0 & 3); const bool def = def_epoch[NBS_K_DS][id] != uint16_t(epoch);
                const uint32_t content = def ? (8 + sizeof(p_bind_state) + 7 & ~7u) + 8 : 0;
                uint8_t *sb = mem.rec(OP_X_NB_STATE, uint32_t(sizeof(nbs_hdr) + sizeof(nbs_blk) + content));
                nbs_hdr h{NBS_VER, 1, 0, epoch}; memcpy(sb, &h, sizeof h); nbs_blk k{NBS_K_DS, uint8_t(def ? NBS_B_DEF : NBS_B_REF), uint16_t(id), content}; memcpy(sb + 8, &k, sizeof k);
                if (def) { uint8_t *c = sb + 16; const uint32_t sz = (8 + sizeof(p_bind_state) + 7) & ~7u; Mem::hdr(c, OP_BIND_STATE, sz); s = p_bind_state{}; s.kind = 5; memcpy(s.bytes, &m.dsb, 8); memcpy(c + 8, &s, sizeof s);
                    Mem::hdr(c + sz, OP_END, 8); def_epoch[NBS_K_DS][id] = uint16_t(epoch); }
            }
            p_bind_vattr va{}; va.first = 0; va.n = 3; memcpy(va.st, m.attr, sizeof m.attr); memcpy(mem.rec(OP_BIND_VATTR, 8 + 12), &va, 8 + 12);
            if (q.upto(2)) { p_bind_vstream vs{}; vs.first = 0; vs.n = 2; vs.st[0] = vs_st{m.stride0, 0}; vs.st[1] = vs_st{m.stride1, 0}; memcpy(mem.rec(OP_BIND_VSTREAM, 8 + 16), &vs, 8 + 16); }
            else { p_bind_vstream vs{}; vs.first = 0; vs.n = 1; vs.st[0] = vs_st{m.stride0, 0}; memcpy(mem.rec(OP_BIND_VSTREAM, 8 + 8), &vs, 8 + 8); vs.first = 1; vs.st[0] = vs_st{m.stride1, 0}; memcpy(mem.rec(OP_BIND_VSTREAM, 8 + 8), &vs, 8 + 8); }
            p_bind_vbuf vb{}; vb.first = 0; vb.n = 2; vb.r[0] = NVNbufferRange{me.vb0, me.vb0s}; vb.r[1] = NVNbufferRange{me.vb1 + alt, me.vb1s}; memcpy(mem.rec(OP_BIND_VBUF, 8 + 32), &vb, 8 + 32);
        }
    }
    void draw(const Mat &m, const Mesh &me, uint32_t texsel) {
        nbd_shadow s; memset(&s, 0, sizeof s);
        const uint32_t nrow = uint32_t(m.fp) % 4;
        for (uint32_t i = 0; i < nrow; i++) { const int slot = int((i * 5 + uint32_t(m.fp)) % 16); if (q.upto(8)) nbd_add(&s, NBD_B_TEX, 1, slot, 0x100 + (texsel + i) % 5, 0); if (!q.upto(3)) nbd_add(&s, NBD_B_SAMP, 1, slot, 0x200 + (texsel >> 2) % 3, 0); }
        if (!q.upto(5)) nbd_add(&s, NBD_B_TEX, 1, 40 + int(q.upto(8)), q.next(), 0);   // a slot no plan reads
        const uint32_t noff = 1 + uint32_t(m.vp) % 3;
        for (uint32_t i = 0; i < noff; i++) { head = (head + 256 * (1 + r.upto(3))) & 0xffffff; nbd_add(&s, NBD_B_UBO, int(i & 1), int(i), ring + head, q.upto(9) ? 256 : 128); }
        s.h.engine_op = 36; s.h.flags = NBD_F_NR; s.h.prim = 4; s.h.count = me.count;
        if (me.indexed) { s.h.draw = NBD_DRAW_IDX; s.h.itype = me.itype; s.h.base_vertex = me.base_vertex; s.h.instances = 1; s.h.ibuf = me.ibuf; } else { s.h.draw = NBD_DRAW_ARRAYS; s.h.first = me.first; }
        if (!q.upto(60)) { s.h.flags |= NBD_F_COND; s.h.cond_addr = 0x7777; }   // a draw with a render condition: never answered, its address stays
        const uint32_t size = nbd_size(&s) + uint32_t(sizeof(nrd_tail)); const uint32_t key = q.upto(70) ? m.key : 0; uint8_t *rc = mem.rec(OP_X_NB_DRAW, size);
        nbd_pack(&s, rc, size); nrd_tail_put(rc, size, key, 1, 0, 3);
    }
    void noise() {
        switch (q.upto(9)) {
        case 0: { p_stencil a{int32_t(1 + q.upto(3)), int32_t(q.upto(3)), int32_t(q.upto(3)), int32_t(q.upto(3)), int32_t(q.upto(3))}; mem.put(OP_STENCIL, a); break; }
        case 1: { p_poly_offset a{float(q.upto(3)), float(q.upto(2)), 0}; mem.put(OP_POLY_OFFSET, a); break; }
        case 2: { p_blend_color a{{float(q.upto(2)), 0, 0, 1}}; mem.put(OP_BLEND_COLOR, a); break; }
        case 3: { p_alpha_ref a{float(q.upto(3)) * 0.5f, 0}; mem.put(OP_X_ALPHA_REF, a); break; }
        case 4: { p_prim_restart a{q.upto(2), 0xffff}; mem.put(OP_X_PRIM_RESTART, a); break; }
        case 5: { p_flag a{int32_t(q.upto(2))}; mem.put(q.upto(2) ? OP_X_DEPTH_CLAMP : OP_X_RAST_DISCARD, a); break; }
        case 6: { p_bind_buf a{int32_t(q.upto(2)), int32_t(q.upto(4)), 0x7400000000ull + 256 * q.upto(4), 256}; mem.put(OP_BIND_SSBO, a); break; }
        case 7: { p_depth_range a{0.f, q.upto(2) ? 1.f : 0.5f}; mem.put(OP_DEPTH_RANGE, a); break; }
        default: { xbg_depth_bounds a{q.upto(2), 0, 1}; mem.put(OP_X_DEPTH_BOUNDS, a); uint32_t ps = 3 + q.upto(2); mem.put(OP_X_PATCH_SIZE, ps); p_copy_stride cs{int64_t(q.upto(3))}; mem.put(q.upto(2) ? OP_COPY_STRIDE : OP_COPY_IMAGE_STRIDE, cs);
            p_bind_handles bh{}; bh.stage = 1; bh.first = int32_t(q.upto(4)); bh.n = 2; bh.h[0] = 0x300 + q.upto(2); bh.h[1] = 0x310; memcpy(mem.rec(OP_BIND_IMG, 12 + 16), &bh, 12 + 16); break; }
        }
    }
    void pass(uint32_t w) {
        p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = 1; rt.color[0] = reinterpret_cast<const tex_t *>(uintptr_t(0x9000 + 64 * (w & 3))); rt.depth = reinterpret_cast<const tex_t *>(uintptr_t(0x9800 + 64 * (w & 1)));
        mem.put(OP_SET_RT, rt); uint32_t z = r.next(); mem.put(OP_T_PASS, z);
        // a pass starts from known values of everything the draws in between may have moved (as an engine's pass setup does)
        for (int wh = 0; wh < 3; wh++) { p_stencil a{3, 0xff, 0xff, 0, wh}; mem.put(OP_STENCIL, a); }
        { p_poly_offset a{0, 0, 0}; mem.put(OP_POLY_OFFSET, a); p_blend_color b{{0, 0, 0, 0}}; mem.put(OP_BLEND_COLOR, b); p_alpha_ref c{0, 0}; mem.put(OP_X_ALPHA_REF, c); p_prim_restart d{0, 0}; mem.put(OP_X_PRIM_RESTART, d);
          p_flag e{0}; mem.put(OP_X_DEPTH_CLAMP, e); mem.put(OP_X_RAST_DISCARD, e); p_depth_range f{0.f, 1.f}; mem.put(OP_DEPTH_RANGE, f); xbg_depth_bounds g{0, 0, 1}; mem.put(OP_X_DEPTH_BOUNDS, g);
          for (int st = 0; st < 2; st++) for (int sl = 0; sl < 4; sl++) { p_bind_buf h{st, sl, 0, 0}; mem.put(OP_BIND_SSBO, h); } }
        p_viewports v{}; v.first = 0; v.n = 1; v.v[0][2] = float(640u << (w & 1)); v.v[0][3] = 360; mem.put(OP_VIEWPORTS, v);
        p_scissor sc{0, 0, int32_t(640u << (w & 1)), 360}; mem.put(OP_SCISSOR, sc);
        if (w % 3 == 0 || !r.upto(40)) { p_set_pool sp{&g_pool[r.upto(40) ? (w >> 1 & 1) : r.upto(2)]}; mem.put(OP_SET_TEX_POOL, sp); sp.pool = &g_pool[2 + (w >> 2 & 1)]; mem.put(OP_SET_SAMP_POOL, sp); }
    }
    Frame frame() {
        ++epoch;
        for (int n = 0; n < 2; n++) if (!r.upto(4)) order[r.upto(uint32_t(order.size()))] = r.upto(240);
        if (!r.upto(20)) ++g.set_gen; if (!r.upto(20)) ++g.pipe_gen; if (!r.upto(25)) g.ring_delta -= 256 * (1 + r.upto(4));
        Frame f; f.g = g;
        uint64_t callee = 0;
        if (!r.upto(3)) { callee = uint64_t(uintptr_t(mem.begin())); p_poly_offset a{5.f, float(r.upto(3)), 0}; mem.put(OP_POLY_OFFSET, a); q.s = 77; material(mats[3], meshes[5]); draw(mats[4], meshes[6], 1); mem.finish(); }
        const uint32_t nh = 1 + r.upto(2); size_t i = 0;
        for (uint32_t h = 0; h < nh; h++) {
            f.handles.push_back(uint64_t(uintptr_t(mem.begin())));
            const size_t to = h + 1 == nh ? order.size() : order.size() / 2; uint32_t w = h;
            pass(w);
            for (; i < to; i++) {
                if (i % 65 == 64) pass(++w);
                if (!r.upto(50)) continue;   // culled this frame
                const uint32_t k = r.upto(400);
                if (k < 3) { uint32_t z = r.next(); mem.put(k ? OP_T_OTHER : OP_T_SILENT, z); }
                else if (k < 5 && callee) { mem.put(OP_CALL, callee); }
                else if (k < 14) {   // a REF whose DEF is of another epoch (or was never made): skipped by both
                    uint8_t *q = mem.rec(OP_X_NB_STATE, uint32_t(sizeof(nbs_hdr) + sizeof(nbs_blk))); nbs_hdr hh{NBS_VER, 1, 0, epoch}; memcpy(q, &hh, sizeof hh);
                    nbs_blk kk{NBS_K_DS, NBS_B_REF, uint16_t(4 + r.upto(4)), 0}; if (r.upto(2)) { kk.id = uint16_t(r.upto(4)); if (def_epoch[NBS_K_DS][kk.id] == uint16_t(epoch)) kk.id = 7; } memcpy(q + 8, &kk, sizeof kk); }
                q.s = r.upto(150) ? (uint64_t(i) + 1) * 0x9E3779B97F4A7C15ull + order[i] : r.next() | 1;
                if (!q.upto(12)) noise();
                const Mat &m = mats[order[i] % 24]; const Mesh &me = meshes[order[i] % 40];
                if (q.upto(6)) material(m, me);   // (now and then a draw runs on the state the draw before left)
                draw(m, me, (order[i] / 24) % 7 + (q.upto(30) ? 0 : 1));
            }
            mem.finish();
        }
        return f;
    }
};

// ---------------- the model of the translation thread: resolution from its State alone ----------------
static uint64_t mix(uint64_t h, uint64_t v) { h = (h ^ v) * 0xBF58476D1CE4E5B9ull; return h ^ h >> 29; }
static uint64_t mixb(uint64_t h, const void *p, size_t n) { return mix(h, nrz8::hash_bytes(p, n)); }
struct Plan { unsigned nrow = 0, noff = 0; uint8_t rns[4], rslot[4], rsamp[4], dst[4], dsl[4]; uint16_t used = 0; uint64_t layout = 0; };
static bool plan_of(const TS &S, Plan &p) {
    if (!S.tprog[0] || !S.tprog[4]) return false;
    const uint32_t vp = uint32_t(S.tprog[0]->serial - 100), fp = uint32_t(S.tprog[4]->serial - 100);
    p.nrow = fp % 4; for (unsigned i = 0; i < p.nrow; i++) { p.rns[i] = 1; p.rslot[i] = uint8_t((i * 5 + fp) % 16); p.rsamp[i] = (fp & 8) ? 0xff : p.rslot[i]; }
    p.noff = 1 + vp % 3; for (unsigned i = 0; i < p.noff; i++) { p.dst[i] = uint8_t(i & 1); p.dsl[i] = uint8_t(i); }
    p.used = (vp & 1) ? 0x3 : 0x1; p.layout = 0x2000 + mix(vp, fp) % 4; return true;
}
struct Answer { uint64_t did = 0, pent = 0; };
// the "NR-08 entry" and the "NR-09 entry" of the draw the State describes (every field such an entry depends on, listed here)
static Answer resolve(const TS &S, const Plan &p, uint32_t key) {
    Answer a; uint64_t h = mix(S.tprog[0]->serial, S.tprog[4]->serial);
    h = mix(h, uint64_t(uint32_t(S.viewport.width)) << 32 | uint32_t(S.viewport.height)); h = mix(h, uint64_t(uintptr_t(S.texture_pool.pool))); h = mix(h, uint64_t(S.texture_pool.off)); h = mix(h, uint64_t(S.texture_pool.n));
    h = mix(h, uint64_t(uintptr_t(S.sampler_pool.pool))); h = mix(h, uint64_t(S.sampler_pool.off));
    for (unsigned i = 0; i < p.nrow; i++) { h = mix(h, S.tex[p.rns[i]][p.rslot[i]]); h = mix(h, p.rsamp[i] != 0xff ? S.samp[p.rns[i]][p.rsamp[i]] : 0); }
    for (unsigned i = 0; i < p.noff; i++) { const uint64_t z = S.ub[p.dst[i]][p.dsl[i]].size; h = mix(h, z < 256 ? z : 256); }
    a.did = h;
    h = mix(key, S.tprog[0]->serial); h = mix(h, S.tprog[4]->serial); h = mixb(h, &S.rt, sizeof S.rt); h = mixb(h, S.attrib, sizeof S.attrib); h = mixb(h, S.stream, sizeof S.stream);
    h = mixb(h, S.blend, sizeof S.blend); h = mixb(h, &S.color, sizeof S.color); h = mixb(h, &S.mask, sizeof S.mask); h = mixb(h, &S.ms, sizeof S.ms); h = mixb(h, S.blend_color, sizeof S.blend_color);
    h = mixb(h, &S.alpha_ref, sizeof S.alpha_ref); h = mixb(h, &S.ds, sizeof S.ds); h = mixb(h, &S.poly, sizeof S.poly); h = mixb(h, S.bias, sizeof S.bias); h = mixb(h, S.stencil_compare, sizeof S.stencil_compare);
    h = mixb(h, S.stencil_write, sizeof S.stencil_write); h = mixb(h, S.stencil_ref, sizeof S.stencil_ref); h = mixb(h, &S.bounds, sizeof S.bounds); h = mixb(h, &S.restart, sizeof S.restart);
    h = mix(h, S.depth_clamp); h = mix(h, S.rast_discard); h = mix(h, S.patch_size); h = mixb(h, S.sb, sizeof S.sb); h = mixb(h, S.img, sizeof S.img); h = mix(h, uint64_t(S.viewport.minDepth * 8) << 8 | uint64_t(S.viewport.maxDepth * 8));
    a.pent = h; return a;
}
// the values of the draw from an answer and what is of the moment (generations, uniform addresses, vertex ranges, viewport)
static bool values(const TS &S, const Plan &p, const Answer &a, const Gens &g, const nbd_hdr &h, nrz5_vals &v, uint32_t *off) {
    memset(&v, 0, sizeof v); v.pipe = 0x1000 + mix(a.pent, g.pipe_gen) % 9; v.layout = p.layout; v.set = 0x3000 + mix(a.did, g.set_gen) % 7; v.noff = p.noff; v.off = off;
    for (unsigned i = 0; i < p.noff; i++) { const uint64_t ad = S.ub[p.dst[i]][p.dsl[i]].address; if (!ad || ad < g.ring_delta || ad - g.ring_delta > 0xffffffffull) return false; off[i] = uint32_t(ad - g.ring_delta); }
    VkViewport vp{S.viewport.x, S.viewport.y, S.viewport.width, S.viewport.height, S.viewport.minDepth, S.viewport.maxDepth}; memcpy(v.vp, &vp, sizeof vp); memcpy(v.sc, &S.scissor, sizeof S.scissor);
    v.used = p.used;
    for (uint32_t m = p.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); if (!S.vb[i].address || !S.vb[i].size) return false; v.vb[i] = 0x4000 + (S.vb[i].address >> 31 & 1) + (S.vb[i].size >> 13 & 1) * 2; v.vbo[i] = S.vb[i].address & 0xffffff; }
    v.indexed = h.draw == NBD_DRAW_IDX; v.count = uint32_t(h.count);
    if (v.indexed) { v.ib = 0x5000; v.ibo = h.ibuf & 0xffffff; v.it = h.itype == 1 ? uint32_t(VK_INDEX_TYPE_UINT16) : uint32_t(VK_INDEX_TYPE_UINT32); v.instances = uint32_t(h.instances); v.first = 0; v.vertex_offset = h.base_vertex; v.first_instance = uint32_t(h.base_instance); }
    else { v.instances = 1; v.first = uint32_t(h.first); v.first_instance = 0; }
    return true;
}

// execute()'s cases for the plain state records, written out here on their own (xbg_game.cpp; `state` spelled S): what nr_z8.h's fold()
// is compared with. false: a record execute() would stop on.
template <class T> static T pl(const uint8_t *p, size_t n) { T o{}; memcpy(&o, p, n < sizeof(T) ? n : sizeof(T)); return o; }
static bool model_state_op(TS &S, uint32_t op, const uint8_t *p, size_t n) {
    switch (op) {
    case OP_SET_RT: S.rt = pl<p_set_rt>(p, n); return n >= sizeof(p_set_rt);
    case OP_VIEWPORTS: { auto a = pl<p_viewports>(p, n); if (!(a.first == 0 && a.n == 1)) return false; S.viewport.x = a.v[0][0]; S.viewport.y = a.v[0][1]; S.viewport.width = a.v[0][2]; S.viewport.height = a.v[0][3]; return true; }
    case OP_SCISSOR: { auto a = pl<p_scissor>(p, n); if (!(a.w >= 0 && a.h >= 0)) return false; S.scissor = {{a.x, a.y}, {uint32_t(a.w), uint32_t(a.h)}}; return true; }
    case OP_DEPTH_RANGE: { auto a = pl<p_depth_range>(p, n); S.viewport.minDepth = a.n; S.viewport.maxDepth = a.f; return true; }
    case OP_BIND_PROGRAM: { auto a = pl<p_bind_program>(p, n); return bind_ts(S, a.prog, a.stages); }
    case OP_BIND_STATE: { auto a = pl<p_bind_state>(p, n);
        switch (a.kind) { case 0: { blend_st b; memcpy(&b, a.bytes, sizeof(b)); S.blend[b.target] = b; break; } case 1: memcpy(&S.color, a.bytes, sizeof(S.color)); break; case 2: memcpy(&S.mask, a.bytes, sizeof(S.mask)); break;
            case 3: memcpy(&S.ms, a.bytes, sizeof(S.ms)); break; case 4: memcpy(&S.poly, a.bytes, sizeof(S.poly)); break; case 5: memcpy(&S.ds, a.bytes, sizeof(S.ds)); break; default: return false; }
        return true; }
    case OP_BIND_VATTR: { auto a = pl<p_bind_vattr>(p, n); if (!(a.first == 0 && a.n >= 0 && a.n <= 16 && n >= offsetof(p_bind_vattr, st) + size_t(a.n) * 4)) return false; return vattr_replace(S.attrib, reinterpret_cast<const uint32_t *>(a.st), a.n) == 0; }
    case OP_BIND_VSTREAM: { auto a = pl<p_bind_vstream>(p, n); if (!(a.first >= 0 && a.n >= 0 && a.first + a.n <= 16 && n >= offsetof(p_bind_vstream, st) + size_t(a.n) * sizeof(vs_st))) return false; memcpy(S.stream + a.first, a.st, a.n * sizeof(vs_st)); return true; }
    case OP_BIND_VBUF: { auto a = pl<p_bind_vbuf>(p, n); if (!(a.first >= 0 && a.n >= 0 && a.first + a.n <= 16 && n >= offsetof(p_bind_vbuf, r) + size_t(a.n) * sizeof(NVNbufferRange))) return false; memcpy(S.vb + a.first, a.r, a.n * sizeof(NVNbufferRange)); return true; }
    case OP_BIND_UBO: case OP_BIND_SSBO: { auto a = pl<p_bind_buf>(p, n); if (!(a.stage >= 0 && a.stage < 6 && a.index >= 0 && a.index < 16)) return false; (op == OP_BIND_UBO ? S.ub : S.sb)[a.stage][a.index] = {a.addr, a.size}; return true; }
    case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: { auto a = pl<p_bind_handles>(p, n);
        if (!(a.stage >= 0 && a.stage < 6 && a.first >= 0 && a.n >= 0 && a.n <= 32 && a.first + a.n <= (op == OP_BIND_IMG ? 16 : 128) && n >= offsetof(p_bind_handles, h) + size_t(a.n) * 8)) return false;
        uint64_t *dst = op == OP_BIND_IMG ? S.img[a.stage] : op == OP_BIND_SAMP ? S.samp[a.stage] : S.tex[a.stage]; memcpy(dst + a.first, a.h, a.n * 8); return true; }
    case OP_STENCIL: { auto a = pl<p_stencil>(p, n); if (!(a.face >= 0 && a.face <= 3 && a.which >= 0 && a.which <= 2)) return false;
        for (unsigned i = 0; i < 2; i++) if (a.face & (1 << i)) { if (a.which == 0) S.stencil_compare[i] = a.value_mask; else if (a.which == 1) S.stencil_write[i] = a.mask; else S.stencil_ref[i] = a.ref; } return true; }
    case OP_POLY_OFFSET: { auto a = pl<p_poly_offset>(p, n); S.bias[0] = a.factor; S.bias[1] = a.units; S.bias[2] = a.clamp; return true; }
    case OP_SET_TEX_POOL: { auto a = pl<p_set_pool>(p, n); if (!a.pool) return false; S.texture_pool = *a.pool; return true; }
    case OP_SET_SAMP_POOL: { auto a = pl<p_set_pool>(p, n); if (!a.pool) return false; S.sampler_pool = *a.pool; return true; }
    case OP_COPY_STRIDE: S.copy_stride = pl<p_copy_stride>(p, n).stride; return S.copy_stride >= 0;
    case OP_COPY_IMAGE_STRIDE: S.copy_image_stride = pl<p_copy_stride>(p, n).stride; return S.copy_image_stride >= 0;
    case OP_BLEND_COLOR: { auto a = pl<p_blend_color>(p, n); memcpy(S.blend_color, a.rgba, 16); return true; }
    case OP_COUNT_: { uint32_t size = pl<uint32_t>(p, n); if (!size) return false; S.patch_size = size; return true; }
    case OP_COUNT_ + 5: S.bounds = pl<xbg_depth_bounds>(p, n); return true;
    case OP_X_PRIM_RESTART: S.restart = pl<p_prim_restart>(p, n); return true;
    case OP_X_DEPTH_CLAMP: S.depth_clamp = pl<p_flag>(p, n).on != 0; return true;
    case OP_X_RAST_DISCARD: S.rast_discard = pl<p_flag>(p, n).on != 0; return true;
    case OP_X_ALPHA_REF: S.alpha_ref = pl<p_alpha_ref>(p, n).ref; return true;
    default: return false;
    }
}

// ---------------- the lane's command section over the real bind cache (as nr_z5r_test.cpp) ----------------
struct BindCache { VkPipeline pipe = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t noff = UINT32_MAX, off[32]{}; bool has_vp = false, has_sc = false; VkViewport vp{}; VkRect2D sc{}; VkBuffer vb[16]{}; VkDeviceSize vbo[16]{};
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ibo = 0; VkIndexType it = VK_INDEX_TYPE_MAX_ENUM; };
static void ref_tail(VkCommandBuffer cmd, BindCache &bc, const nrz5_vals &v) {
    const VkPipeline pipe = hnd<VkPipeline>(v.pipe); const VkPipelineLayout layout = hnd<VkPipelineLayout>(v.layout); const VkDescriptorSet set = hnd<VkDescriptorSet>(v.set);
    VkViewport vp; VkRect2D sc; memcpy(&vp, v.vp, sizeof vp); memcpy(&sc, v.sc, sizeof sc); VkBuffer vb[16]; for (int i = 0; i < 16; i++) vb[i] = hnd<VkBuffer>(v.vb[i]);
    if (bc.pipe != pipe) { nbmt::CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); bc.pipe = pipe; }
    { const uint32_t no = v.noff;
      if (bc.layout != layout || bc.set != set || bc.noff != no || no > 32 || (no && memcmp(bc.off, v.off, no * 4))) {
        nbmt::CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, no, v.off);
        if (no <= 32) { bc.layout = layout; bc.set = set; bc.noff = no; if (no) memcpy(bc.off, v.off, no * 4); } else bc.noff = UINT32_MAX; } }
    if (!bc.has_vp || memcmp(&bc.vp, &vp, sizeof vp)) { nbmt::CmdSetViewport(cmd, 0, 1, &vp); bc.vp = vp; bc.has_vp = true; }
    if (!bc.has_sc || memcmp(&bc.sc, &sc, sizeof sc)) { nbmt::CmdSetScissor(cmd, 0, 1, &sc); bc.sc = sc; bc.has_sc = true; }
    { unsigned changed = 0; for (uint32_t m = v.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); if (bc.vb[i] != vb[i] || bc.vbo[i] != v.vbo[i]) changed |= 1u << i; }
      if (changed) { uint8_t f[8], k[8]; const unsigned runs = nr_vb_runs(changed, v.used, f, k);
        for (unsigned r = 0; r < runs; r++) { VkDeviceSize o[16]; for (unsigned j = 0; j < k[r]; j++) { o[j] = v.vbo[f[r] + j]; bc.vb[f[r] + j] = vb[f[r] + j]; bc.vbo[f[r] + j] = v.vbo[f[r] + j]; }
            nbmt::CmdBindVertexBuffers(cmd, f[r], k[r], &vb[f[r]], o); } } }
    if (v.indexed) {
        const VkBuffer ib = hnd<VkBuffer>(v.ib);
        if (bc.ib != ib || bc.ibo != v.ibo || bc.it != VkIndexType(v.it)) { nbmt::CmdBindIndexBuffer(cmd, ib, v.ibo, VkIndexType(v.it)); bc.ib = ib; bc.ibo = v.ibo; bc.it = VkIndexType(v.it); }
        nbmt::CmdDrawIndexed(cmd, v.count, v.instances, v.first, v.vertex_offset, v.first_instance); }
    else nbmt::CmdDraw(cmd, v.count, v.instances, v.first, v.first_instance);
}
static void leave(BindCache &bc, const nrz5_vals &v) {
    bc.pipe = hnd<VkPipeline>(v.pipe);
    if (v.noff <= 32) { bc.layout = hnd<VkPipelineLayout>(v.layout); bc.set = hnd<VkDescriptorSet>(v.set); bc.noff = v.noff; if (v.noff) memcpy(bc.off, v.off, size_t(v.noff) * 4); } else bc.noff = UINT32_MAX;
    memcpy(&bc.vp, v.vp, sizeof bc.vp); bc.has_vp = true; memcpy(&bc.sc, v.sc, sizeof bc.sc); bc.has_sc = true;
    for (uint32_t m = v.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); bc.vb[i] = hnd<VkBuffer>(v.vb[i]); bc.vbo[i] = v.vbo[i]; }
    if (v.indexed) { bc.ib = hnd<VkBuffer>(v.ib); bc.ibo = v.ibo; bc.it = VkIndexType(v.it); }
}
static nrz5_cache conv(const BindCache &bc) {
    nrz5_cache c; nrz5_cache_unknown(&c); c.known = NRZ5_K_PIPE | NRZ5_K_SET | NRZ5_K_VP | NRZ5_K_SC | NRZ5_K_IB; c.known_vb = 0xffff;
    c.pipe = uint64_t(uintptr_t(bc.pipe)); c.layout = uint64_t(uintptr_t(bc.layout)); c.set = uint64_t(uintptr_t(bc.set)); c.noff = bc.noff; memcpy(c.off, bc.off, sizeof c.off);
    c.has_vp = bc.has_vp; c.has_sc = bc.has_sc; memcpy(c.vp, &bc.vp, sizeof bc.vp); memcpy(c.sc, &bc.sc, sizeof bc.sc);
    for (int i = 0; i < 16; i++) { c.vb[i] = uint64_t(uintptr_t(bc.vb[i])); c.vbo[i] = bc.vbo[i]; }
    c.ib = uint64_t(uintptr_t(bc.ib)); c.ibo = bc.ibo; c.it = uint32_t(bc.it); return c;
}

// ---------------- the submitting thread: the walk (the handler of xbg_nr_z8.inc) ----------------
static nrz5r::Ring g_ring; static nrz8::Table g_tab;
struct Walk {
    Sh S; nrz8::Fix fix; nrz8::Walker<Walk> W; nrz5r::Writer wr; bool bad = false; uint8_t stamp = 0; uint64_t draws = 0, values = 0, miss[nrz8::M_N] = {}, fix_stale = 0;
    std::vector<uint64_t> *dig = nullptr;
    struct Acc { const Sh &S;
        bool ub(unsigned st, unsigned sl, uint64_t *a, uint64_t *z) const { if (st >= 6 || sl >= 16) return false; *a = S.ub[st][sl].address; *z = S.ub[st][sl].size; return true; }
        bool vb(unsigned i, uint64_t *a, uint64_t *z) const { if (i >= 16) return false; *a = S.vb[i].address; *z = S.vb[i].size; return true; }
        uint64_t tex(unsigned ns, unsigned sl) const { return ns < 6 && sl < 128 ? S.tex[ns][sl] : ~0ull; } uint64_t samp(unsigned ns, unsigned sl) const { return ns < 6 && sl < 128 ? S.samp[ns][sl] : ~0ull; } };
    void state_op(uint32_t op, const uint8_t *p, size_t n) { const unsigned d = nrz8::fold(S, op, p, n, [this](const prog_t *pr, int32_t st) { return bind_sh(S, pr, st); }, vattr_replace); if (d & nrz8::D_BAD) bad = true; else fix.mark(d); }
    void nr_state(const uint8_t *p, size_t n) { if (n < sizeof(nr3_hdr)) { bad = true; return; } if (p[1]) return; if (apply_sh(&S, p, n) != 0) { bad = true; return; } fix.mark(nrz8::dirty_of_nr3(p, n)); }
    void other(uint32_t) { wr.end_run(); }
    void nb_draw(uint8_t *rec, size_t n, bool top) {
        nbd_hdr h; const uint8_t *b = nullptr; if (nbd_view(rec, n, &h, &b) != 0) { bad = true; return; }
        unsigned ts = 0, bs = 0; if (nbd_apply(&h, b, S.tex, S.samp, S.img, reinterpret_cast<nbd_range (*)[16]>(S.ub), &ts, &bs) != 0) { bad = true; return; }
        ++draws; if (dig) dig->push_back(digest(S));
        nrd_tail t; const bool nr = nrd_tail_get(rec, n, &h, &t) != 0; uint32_t mark = 0; int32_t slot1 = 0; uint64_t ca = 0;
        if (!top || !nr || !t.key || (h.flags & (NBD_F_REN | NBD_F_COND)) || h.count <= 0) wr.no_values();
        else {
            nrz5_site site; nrz5_site_make(&site, t.key, h.draw == NBD_DRAW_IDX ? NRZ5S_IDX : NRZ5S_ARRAYS, h.prim, h.itype, h.count, h.base_vertex, h.base_instance, h.instances, h.first, h.ibuf, S.vb[0].address);
            const uint32_t fixid = fix.get(S, S.sprog); if (fixid != fix.fresh(S, S.sprog)) ++fix_stale;
            const uint32_t s = g_tab.slot_of(&site, fixid); slot1 = int32_t(s + 1); ca = fixid;
            uint64_t w[nrz8::MAX_WORDS]; uint32_t ver = 0; nrz5_vals v; uint32_t off[nrz8::MAX_DYN];
            if (!g_tab.get(s, w, &ver)) wr.no_values();
            else { const int m = nrz8::match(w, &site, fixid, h.count, h.instances, h.first, h.base_vertex, h.base_instance, Acc{S}, &v, off);
                if (m) { ++miss[m]; wr.no_values(); }
                else { ++values; ca |= uint64_t(ver) << 32; mark = wr.draw(v, [](nbmtq::Chunk *ch, nrz5_cache *mm, const nrz5_vals *vv) { nbmt::Build bb{ch}; nbmt::Z5Sink sink{bb}; return nrz5_emit(sink, mm, vv); }); } }
        }
        rec[offsetof(nbd_hdr, _pad0)] = stamp; memcpy(rec + offsetof(nbd_hdr, _pad1), &mark, 4); memcpy(rec + offsetof(nbd_hdr, _pad2), &slot1, 4);
        if (!(h.flags & NBD_F_COND)) memcpy(rec + offsetof(nbd_hdr, cond_addr), &ca, 8);
    }
    void submit(const Frame &f, uint64_t sub) { stamp = nrz8::stamp_of(sub); uint64_t budget = 0; for (uint64_t hd : f.handles) { if (!W.walk(reinterpret_cast<const uint8_t *>(uintptr_t(hd)), 0, budget, true)) bad = true; wr.end_run(); } }
};

// ---------------- the translation thread ----------------
struct Priv { uint32_t ver = 0; Answer a; uint32_t set_gen = 0, pipe_gen = 0; };
struct Tot { uint64_t draws = 0, recorded = 0, valid = 0, answered = 0, stale = 0, a_cmp = 0, a_bad = 0, dig_cmp = 0, published = 0; nrz5r::RStats r; uint64_t w_draws = 0, w_values = 0, w_tails = 0, w_fix_stale = 0, w_miss[nrz8::M_N] = {}; } tot;
static nrz5r::Reader *g_R = nullptr;
static void pre_hook() { if (g_R && g_R->holding) g_R->flush(nrz5r::CUT_WRITE); }
static uint64_t sig() { const nbmtq::Stats &q = nbmt::q.st; return q.cmds * 8 + q.sync[nbmtq::NBMT_SY_PNEXT] + q.sync[nbmtq::NBMT_SY_ALLOC] + q.sync[nbmtq::NBMT_SY_CALLER]; }
static bool is_draw(uint16_t op) { return op == nbmt::OP_Draw || op == nbmt::OP_DrawIndexed; }
struct Xl {   // one run of the model: mode 0 today, 1 apply, 2 verify
    TS S; nrz8::Def defs[NBS_KINDS][NBS_MAX_IDS] = {}; int mode = 0; nrz5r::Reader *R = nullptr; std::vector<Priv> priv; Gens g{}; uint8_t stamp = 0;
    VkCommandBuffer c{}; BindCache bc; const std::vector<uint64_t> *dig = nullptr; size_t dig_at = 0; std::string *outcome = nullptr;
    void other(uint32_t op, const uint8_t *p) { uint32_t z; memcpy(&z, p, 4);
        if (op == OP_T_SILENT) { bc = BindCache{}; return; }
        if (op == OP_T_PASS) { nbmt::CmdEndRenderPass(c); VkRenderPassBeginInfo b{}; b.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; b.renderPass = hnd<VkRenderPass>(z & 7); b.renderArea.extent = {640, 360}; nbmt::CmdBeginRenderPass(c, &b, VK_SUBPASS_CONTENTS_INLINE); bc = BindCache{}; }
        else if (op == OP_T_OTHER) nbmt::CmdFillBuffer(c, hnd<VkBuffer>(0x77), z & 0xff0, 16, z); }
    void publish(const nbd_hdr &h, uint32_t key, const Plan &p, const Answer &a, const nrz5_vals &v) {
        if (mode == 0 || h._pad2 <= 0 || uint32_t(h._pad2 - 1) >= g_tab.n) return;
        nrz8::Desc d; nrz5_site_make(&d.site, key, h.draw == NBD_DRAW_IDX ? NRZ5S_IDX : NRZ5S_ARRAYS, h.prim, h.itype, h.count, h.base_vertex, h.base_instance, h.instances, h.first, h.ibuf, S.vb[0].address);
        d.fixid = uint32_t(h.cond_addr); d.v = &v; d.first_index = 0; d.k = 0; d.noff = p.noff;
        for (unsigned i = 0; i < p.noff; i++) { const NVNbufferRange &u = S.ub[p.dst[i]][p.dsl[i]]; d.dyn[i].st = p.dst[i]; d.dyn[i].sl = p.dsl[i]; d.dyn[i].addr = u.address; d.dyn[i].rbytes = 256; d.dyn[i].copy = uint32_t(u.size < 256 ? u.size : 256); }
        unsigned nvb = 0; for (uint32_t m = v.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); d.vb[nvb].at = uint8_t(i); d.vb[nvb].addr = S.vb[i].address; d.vb[nvb].size = S.vb[i].size; ++nvb; }
        d.nvb = nvb; d.nrow = p.nrow; for (unsigned i = 0; i < p.nrow; i++) { d.row[i].ns = p.rns[i]; d.row[i].slot = p.rslot[i]; d.row[i].samp = p.rsamp[i]; d.row[i].tex = S.tex[p.rns[i]][p.rslot[i]]; d.row[i].smp = p.rsamp[i] != 0xff ? S.samp[p.rns[i]][p.rsamp[i]] : 0; }
        uint64_t w[nrz8::MAX_WORDS]; const unsigned nw = nrz8::pack(d, w); if (!nw) return;
        Priv &pv = priv[h._pad2 - 1]; const uint32_t nv = g_tab.put(uint32_t(h._pad2 - 1), w, nw); if (nv != pv.ver) ++tot.published; pv.ver = nv; pv.a = a; pv.set_gen = g.set_gen; pv.pipe_gen = g.pipe_gen;
    }
    void draw(const uint8_t *rec, size_t n) {
        nbd_hdr h; const uint8_t *b = nullptr; CHECK(nbd_view(rec, n, &h, &b) == 0, "record"); unsigned ts = 0, bs = 0; nbd_apply(&h, b, S.tex, S.samp, S.img, reinterpret_cast<nbd_range (*)[16]>(S.ub), &ts, &bs);
        if (dig && dig_at < dig->size()) { ++tot.dig_cmp; CHECK((*dig)[dig_at] == digest(S), "the walk's copy is State at draw record %zu", dig_at); ++dig_at; }
        nrd_tail t{}; nrd_tail_get(rec, n, &h, &t); const uint32_t key = t.key; ++tot.draws;
        const bool mine = mode && h._pad0 == stamp; const uint32_t mw = mine ? h._pad1 : 0u, ver = mine && !(h.flags & NBD_F_COND) ? uint32_t(h.cond_addr >> 32) : 0u; if (!mine) h._pad2 = 0;
        Plan p; const bool dropped = !plan_of(S, p) || mix(key, S.tprog[4]->serial) % 37 == 0;   // the prologue of draw() drops it (a function of State)
        auto miss = [&] { if (!mode) return; if (R->holding) { if (mode == 2) R->verify_miss(); else R->flush(nrz5r::CUT_RECORD); } else R->abandon(mw); };
        if (dropped) { miss(); if (outcome) outcome->push_back('D'); return; }
        Answer a; bool answered = false;
        if (mode == 1 && ver && h._pad2 > 0 && priv[h._pad2 - 1].ver == ver) { const Priv &pv = priv[h._pad2 - 1];
            if (pv.set_gen == g.set_gen && pv.pipe_gen == g.pipe_gen) { a = pv.a; answered = true; ++tot.answered; } else ++tot.stale; }   // (State is NOT asked: the answer must be right by itself)
        if (!answered) a = resolve(S, p, key);
        if (mode == 2 && ver && h._pad2 > 0 && priv[h._pad2 - 1].ver == ver && priv[h._pad2 - 1].set_gen == g.set_gen && priv[h._pad2 - 1].pipe_gen == g.pipe_gen) { ++tot.a_cmp;
            if (priv[h._pad2 - 1].a.did != a.did || priv[h._pad2 - 1].a.pent != a.pent) { ++tot.a_bad; CHECK(false, "verify: the walk's answer is the model's (key %u)", key); } }
        nrz5_vals v; uint32_t off[8];
        if (!values(S, p, a, g, h, v, off)) { miss(); if (outcome) outcome->push_back('D'); return; }
        const bool foreign = key % 41 == 0;
        if (!mode || nbmt::cur != c) { if (mode) R->abandon(mw); ref_tail(c, bc, v); ++tot.recorded; if (mode) publish(h, key, p, a, v); if (foreign) { const VkPipeline fp = hnd<VkPipeline>(0x1999); nbmt::CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, fp); bc.pipe = fp; } if (outcome) outcome->push_back('R'); return; }
        auto holds = [&](const nrz5_cache &sh, const nrz5_vals &vv) { const nrz5_cache real = conv(bc); return nrz5r::cache_holds(&sh, &real, &vv); };
        char o = 'R';
        if (mode == 2) {
            const bool would = R->verify_begin(mw, v, holds, is_draw, sig()); if (would) { ++tot.valid; o = 'V'; }
            nbmtq::Chunk *c0 = nbmt::q.cur; const uint32_t u0 = c0 ? c0->used : 0; const uint64_t ch0 = nbmt::q.st.chunks;
            ref_tail(c, bc, v); ++tot.recorded;
            nbmtq::Chunk *cc = c0 ? c0 : nbmt::q.cur; const uint32_t from = c0 ? u0 : 0; const bool readable = cc && cc == nbmt::q.cur && ch0 == nbmt::q.st.chunks;
            R->verify_end(readable ? cc->data() + from : nullptr, readable ? cc->used - from : 0, [](const uint8_t *x, uint32_t na, const uint8_t *y, uint32_t nb) { uint32_t at = 0; return nbmt::same_stream(x, na, y, nb, &at); }, sig());
            publish(h, key, p, a, v);
        } else if (R->draw(mw, v, holds, is_draw) == nrz5r::Reader::VALID) { leave(bc, v); ++tot.valid; o = 'V'; }
        else { ref_tail(c, bc, v); ++tot.recorded; publish(h, key, p, a, v); }
        if (foreign) { const VkPipeline fp = hnd<VkPipeline>(0x1999); nbmt::CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, fp); bc.pipe = fp; }
        if (outcome) outcome->push_back(o);
    }
    // execute(): its own traversal
    void exec(const uint8_t *pc, unsigned depth) {
        CHECK(depth < 64, "depth");
        for (;;) {
            op_hdr hd; memcpy(&hd, pc, 8); const uint8_t *p = pc + 8; const size_t n = hd.size - 8;
            if (hd.op == OP_END) return;
            if (hd.op == OP_JUMP) { uint64_t t; memcpy(&t, p, 8); pc = reinterpret_cast<const uint8_t *>(uintptr_t(t)); continue; }
            if (hd.op == OP_CALL) { uint64_t t; memcpy(&t, p, 8); exec(reinterpret_cast<const uint8_t *>(uintptr_t(t)), depth + 1); }
            else if (hd.op == OP_X_NB_STATE) { nbs_hdr sh; nbs_view v[NBS_KINDS]; const int nb = nbs_parse(p, n, &sh, v); CHECK(nb > 0, "state record");
                for (int i = 0; i < nb; i++) { const uint8_t *cn = v[i].content; nrz8::Def &d = defs[v[i].b.kind][v[i].b.id];
                    if (v[i].b.how == NBS_B_REF) { if (d.epoch != sh.epoch || !d.p) continue; cn = d.p; } else if (v[i].b.how == NBS_B_DEF) d = nrz8::Def{sh.epoch, cn};
                    exec(cn, depth + 1); } }
            else if (hd.op == OP_X_NR_STATE) { if (!p[1]) CHECK(apply_ts(&S, p, n) == 0, "state packet"); }
            else if (hd.op == OP_X_NB_DRAW) draw(p, n);
            else if (nrz8::is_state_op(hd.op)) CHECK(model_state_op(S, hd.op, p, n), "a state record the model refuses (op %u)", hd.op);
            else other(hd.op, p);
            pc += hd.size;
        }
    }
};

struct Cfg { int mode; unsigned flush, chunk_kb, queue; int slow; };
static void setup(const Cfg &cfg) {
    char e[4][64]; snprintf(e[0], 64, "%d", cfg.mode); snprintf(e[1], 64, "%u", cfg.flush); snprintf(e[2], 64, "%u", cfg.chunk_kb); snprintf(e[3], 64, "%u", cfg.queue);
#if defined(_WIN32)
    _putenv_s("DM_NB_MT", e[0]); _putenv_s("DM_NB_MT_FLUSH", e[1]); _putenv_s("DM_NB_MT_CHUNK_KB", e[2]); _putenv_s("DM_NB_MT_QUEUE", e[3]);
#else
    setenv("DM_NB_MT", e[0], 1); setenv("DM_NB_MT_FLUSH", e[1], 1); setenv("DM_NB_MT_CHUNK_KB", e[2], 1); setenv("DM_NB_MT_QUEUE", e[3], 1);
#endif
    nbmt::init(); g_slow.store(cfg.slow);
    { const uint32_t n = (32u << 20) / nbmt::q.chunk_bytes; nbmt::q.spare_max = n < 8 ? 8 : n > 1024 ? 1024 : n; }
}
static void finish() { nbmt::stop(); g_slow.store(0); nbmt::q.st = nbmtq::Stats{}; { std::lock_guard<std::mutex> l(nbmt::q.mu); nbmt::q.ws = nbmtq::WorkerStats{}; CHECK(!nbmt::q.running && !nbmt::q.free_list && !nbmt::q.head, "stop: worker gone, chunk memory returned"); } }

struct Item { size_t f; std::vector<uint64_t> dig; std::unique_ptr<Sh> end; };
struct Handoff { std::mutex mu; std::condition_variable cv; std::deque<Item> q; bool done = false; };
// One run of all frames. mode 0: today (no walk). Returns the logs; checks the fold after every frame.
static std::vector<std::vector<uint8_t>> run(const std::vector<Frame> &prog, int mode, uint64_t seed, std::string *outcome = nullptr, bool threads = true) {
    std::vector<std::vector<uint8_t>> logs(prog.size()); tot = Tot{};
    auto X = std::make_unique<Xl>(); X->mode = mode; X->outcome = outcome; nrz5r::Reader R; R.q = &nbmt::q; R.ring = &g_ring; X->R = &R; Rng r{seed * 31 + 3};
    if (mode) { CHECK(g_tab.init(8192), "table"); X->priv.resize(g_tab.n); g_pre = mode == 1 ? pre_hook : nullptr; g_R = mode == 1 ? &R : nullptr; }
    auto W = std::make_unique<Walk>(); Handoff h; std::thread gt;
    auto walk_one = [&](size_t f) { Item it; it.f = f; if (mode) { W->dig = &it.dig; W->submit(prog[f], f + 1); W->dig = nullptr; it.end.reset(new Sh(W->S)); } return it; };
    if (mode) { CHECK(W->fix.init() && W->W.init(), "walk"); W->W.h = W.get(); W->wr.q = &nbmt::q; W->wr.ring = &g_ring; W->wr.min_tails = 1 + uint32_t(seed % 3); }
    if (mode && threads) gt = std::thread([&] { for (size_t f = 0; f < prog.size(); f++) { Item it = walk_one(f); std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return h.q.size() < 3; }); h.q.push_back(std::move(it)); h.cv.notify_all(); }
        { std::lock_guard<std::mutex> l(h.mu); h.done = true; } h.cv.notify_all(); });
    for (size_t f = 0; f < prog.size(); f++) {
        Item it;
        if (mode && threads) { std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return !h.q.empty() || h.done; }); CHECK(!h.q.empty(), "a frame for every walk"); if (h.q.empty()) break; it = std::move(h.q.front()); h.q.pop_front(); h.cv.notify_all(); }
        else it = walk_one(f);
        const bool direct = mode == 1 && r.upto(16) == 2;   // this recording goes by direct calls: nothing written ahead is used
        Log lg; X->c = reinterpret_cast<VkCommandBuffer>(&lg); X->bc = BindCache{}; X->g = prog[f].g; X->stamp = nrz8::stamp_of(f + 1); X->dig = mode ? &it.dig : nullptr; X->dig_at = 0;
        nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(X->c, !direct, false);
        for (uint64_t hd : prog[f].handles) { X->exec(reinterpret_cast<const uint8_t *>(uintptr_t(hd)), 0); if (mode) { if (R.holding) { if (mode == 2) R.verify_miss(); else R.flush(nrz5r::CUT_RECORD); } } }
        nbmt::end(X->c); logs[f] = std::move(lg.b);
        CHECK(!R.holding, "a run does not outlive its command buffer");
        if (mode) { CHECK(X->dig_at == it.dig.size(), "every draw record the walk saw was executed (%zu of %zu)", X->dig_at, it.dig.size());
            const char *d = state_diff(*it.end, X->S); CHECK(!d, "frame %zu: the walk's copy differs from State in %s", f, d ? d : "");
            for (unsigned st = 0; st < 6; st++) CHECK(it.end->sprog[st] == (X->S.tprog[st] ? X->S.tprog[st]->serial : 0), "frame %zu: program word of stage %u", f, st); }
    }
    if (gt.joinable()) gt.join();
    if (mode) { R.drop(); g_R = nullptr; g_pre = nullptr; tot.r = R.st; tot.w_draws = W->draws; tot.w_values = W->values; tot.w_tails = W->wr.st.tails; tot.w_fix_stale = W->fix_stale; memcpy(tot.w_miss, W->miss, sizeof tot.w_miss);
        CHECK(!W->bad, "the walk followed every record"); CHECK(W->fix_stale == 0, "the fix id is the id of the copy's bytes at every draw (%llu stale)", (unsigned long long)W->fix_stale);
        CHECK(nbmt::q.spare_out.load() == 0, "every spare chunk came back (%u out)", nbmt::q.spare_out.load()); CHECK(g_ring.front() == nullptr, "the ring is empty");
        W->fix.destroy(); W->W.destroy(); g_tab.destroy(); }
    return logs;
}

// ---------------- the table under two readers ----------------
static void table_race() {
    nrz8::Table t; CHECK(t.init(16), "table"); std::atomic<bool> stop{false}; std::atomic<uint64_t> good{0}, torn{0};
    auto make = [](uint64_t seed, uint64_t *w) { const unsigned nw = nrz8::HEAD_WORDS + 3 * unsigned(seed % 5); w[0] = uint64_t(nw) | 1u << 16 | seed << 32; uint64_t x = seed; for (unsigned i = 1; i + 1 < nw; i++) { x = mix(x, i); w[i] = x; } w[nw - 1] = x ^ 0x5555; return nw; };
    auto reader = [&] { uint64_t w[nrz8::MAX_WORDS], e[nrz8::MAX_WORDS]; uint32_t ver; while (!stop.load(std::memory_order_relaxed)) { if (!t.get(3, w, &ver)) continue; const unsigned nw = make(w[0] >> 32, e);
        if (nrz8::words_of(w[0]) == nw && !memcmp(w, e, nw * 8)) good.fetch_add(1, std::memory_order_relaxed); else torn.fetch_add(1, std::memory_order_relaxed); } };
    std::thread a(reader), b(reader); uint64_t w[nrz8::MAX_WORDS]; uint32_t last = 0;
    for (uint64_t i = 1; i <= 400000; i++) { const unsigned nw = make(i % 7 + 1, w); const uint32_t v = t.put(3, w, nw); CHECK(!(v & 1) && v != last, "a new even version a put"); last = v; }
    while (good.load() < 1000 && torn.load() == 0) std::this_thread::yield();
    stop.store(true); a.join(); b.join();
    CHECK(torn.load() == 0, "no torn template (%llu torn, %llu whole)", (unsigned long long)torn.load(), (unsigned long long)good.load()); CHECK(good.load() > 0, "readers read");
    printf("table: 400000 puts, %llu whole reads by two readers, torn %llu\n", (unsigned long long)good.load(), (unsigned long long)torn.load());
    t.destroy();
}
// ---------------- the dictionary: ids by bytes, never handed out twice ----------------
static void dict_test() {
    nrz8::Dict d; CHECK(d.init(256, 4096), "dict"); Rng r{99}; std::vector<std::pair<std::vector<uint8_t>, uint32_t>> seen; uint64_t maxid = 0;
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> b(8 + r.upto(40)); for (auto &x : b) x = uint8_t(r.upto(3)); if (r.upto(3) && !seen.empty()) b = seen[r.upto(uint32_t(seen.size()))].first;
        const uint64_t c0 = d.clears; const uint32_t id = d.id_of(b.data(), uint32_t(b.size())); CHECK(id != 0, "an id");
        if (d.clears != c0) seen.clear();
        bool known = false; for (auto &s : seen) if (s.first == b) { known = true; CHECK(s.second == id, "the same bytes keep their id"); } else CHECK(s.second != id, "other bytes have another id");
        if (!known) { CHECK(id > maxid, "a new content gets an id never handed out before"); seen.push_back({b, id}); } if (id > maxid) maxid = id;
    }
    CHECK(d.clears > 0, "the dictionary filled up and was emptied (%llu times)", (unsigned long long)d.clears); d.destroy();
}

int main(int argc, char **argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 60;
    for (int i = 0; i < 12; i++) { memset(&g_prog[i], 0, sizeof g_prog[i]); g_prog[i].nstage = 2; g_prog[i].serial = 100 + uint64_t(i); }
    for (int i = 0; i < 4; i++) { memset(&g_mem[i], 0, sizeof g_mem[i]); g_pool[i].pool = &g_mem[i]; g_pool[i].off = 64 * i; g_pool[i].n = 100 + i; }
    dict_test(); table_race();
    uint64_t taken = 0, answered = 0, draws = 0, vcmp = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        World w(seed); std::vector<Frame> prog; for (int i = 0; i < frames; i++) prog.push_back(w.frame());
        setup(Cfg{0, 256, 64, 64, 0}); const auto ref = run(prog, 0, seed); finish(); const uint64_t ref_draws = tot.recorded;
        const Cfg shapes[] = {{1, 256, 64, 64, 0}, {2, 256, 64, 64, 0}, {3, 256, 64, 64, 0}, {2, 16, 16, 2, 0}, {3, 16, 16, 2, 7}, {3, 65536, 1024, 1024, 0}};
        for (const Cfg &cf : shapes) {
            const bool first = &cf == &shapes[0] && frames >= 40;   // (the shares are asked of a run long enough to warm the table up)
            setup(cf); { const auto logs = run(prog, 1, seed * 77 + cf.mode);
                for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "apply: seed %llu queue mode %d flush %u: frame %zu's call log is today's (%zu / %zu bytes)", (unsigned long long)seed, cf.mode, cf.flush, f, logs[f].size(), ref[f].size()); }
            const Tot t = tot; finish();
            CHECK(t.valid + t.recorded == ref_draws, "apply: every draw taken or recorded once (%llu + %llu of %llu)", (unsigned long long)t.valid, (unsigned long long)t.recorded, (unsigned long long)ref_draws);
            CHECK(first ? t.answered > t.draws / 4 && t.valid > t.draws / 8 : t.answered > 0 && t.valid > 0, "apply: the walk's answers are used (%llu answered, %llu taken of %llu)", (unsigned long long)t.answered, (unsigned long long)t.valid, (unsigned long long)t.draws);
            taken += t.valid; answered += t.answered; draws += t.draws;
            setup(cf); { const auto logs = run(prog, 2, seed * 177 + cf.mode);
                for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "verify: seed %llu queue mode %d: frame %zu's call log is today's", (unsigned long long)seed, cf.mode, f); }
            const Tot v = tot; finish();
            CHECK(v.a_bad == 0 && (first ? v.a_cmp > v.draws / 4 : v.a_cmp > 0), "verify: answers compared %llu, bad %llu", (unsigned long long)v.a_cmp, (unsigned long long)v.a_bad);
            CHECK(v.r.v_bad == 0 && v.r.v_cmp > 0 && v.r.v_cmp == v.r.v_same, "verify: packets compared %llu, same %llu, bad %llu", (unsigned long long)v.r.v_cmp, (unsigned long long)v.r.v_same, (unsigned long long)v.r.v_bad);
            vcmp += v.r.v_cmp;
            if (&cf == &shapes[0]) printf("seed %llu: %llu draw records (%llu drawn); apply: answered without State %llu (%.0f%%), packets taken %llu (%.0f%%), runs %llu complete %llu, values differ %llu, cache %llu; "
                "walk: values for %llu of %llu, misses site %llu fix %llu tex %llu vb %llu ubo %llu; verify: answers %llu bad %llu, packets %llu bad %llu, digests %llu\n", (unsigned long long)seed,
                (unsigned long long)t.draws, (unsigned long long)ref_draws, (unsigned long long)t.answered, 100. * double(t.answered) / double(t.draws), (unsigned long long)t.valid, 100. * double(t.valid) / double(t.draws),
                (unsigned long long)t.r.started, (unsigned long long)t.r.complete, (unsigned long long)t.r.cut[nrz5r::CUT_VALUES], (unsigned long long)t.r.cut[nrz5r::CUT_CACHE], (unsigned long long)t.w_values, (unsigned long long)t.w_draws,
                (unsigned long long)t.w_miss[nrz8::M_SITE], (unsigned long long)t.w_miss[nrz8::M_FIX], (unsigned long long)t.w_miss[nrz8::M_TEX], (unsigned long long)t.w_miss[nrz8::M_VB], (unsigned long long)t.w_miss[nrz8::M_UBO],
                (unsigned long long)v.a_cmp, (unsigned long long)v.a_bad, (unsigned long long)v.r.v_cmp, (unsigned long long)v.r.v_bad, (unsigned long long)v.dig_cmp);
        }
    }
    // a script with exact outcomes: the same frame three times, one thread
    { World w(7); std::vector<Frame> prog; Frame f = w.frame(); f.g = Gens{1, 1, 0x7300000000ull - 4096}; prog.push_back(f); prog.push_back(f); prog.push_back(f);
      setup(Cfg{1, 256, 64, 64, 0}); std::string o; run(prog, 1, 5, &o, false); const Tot t = tot; finish();
      const size_t per = o.size() / 3; CHECK(o.size() % 3 == 0 && per > 100, "script: three equal frames (%zu outcomes)", o.size());
      CHECK(o.substr(0, per).find('V') == std::string::npos, "script: nothing is taken in the first frame (no template yet)");
      size_t v2 = 0, v3 = 0; for (size_t i = 0; i < per; i++) { v2 += o[per + i] == 'V'; v3 += o[2 * per + i] == 'V'; CHECK((o[per + i] == 'D') == (o[2 * per + i] == 'D'), "script: the same draws are dropped in equal frames"); }
      CHECK(v3 > per / 3 && v3 >= v2, "script: the third frame is taken from the templates of the two before (%zu, then %zu of %zu)", v2, v3, per);
      printf("script: %zu draws a frame, taken 0 / %zu / %zu; answered %llu\n", per, v2, v3, (unsigned long long)t.answered); }
    printf("nr_z8_test: %ld checks, %ld failed; %llu draw records, %llu answered without State, %llu packets taken, %llu packets compared by verify\n", checks, fails, (unsigned long long)draws, (unsigned long long)answered,
           (unsigned long long)taken, (unsigned long long)vcmp);
    return fails ? 1 : 0;
}
