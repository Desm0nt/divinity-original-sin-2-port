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
//   THREAD   (DM_NR_Z8_THREAD=1, NR-ZERO.md section 28) the walk runs on a THIRD thread while the translation thread executes the same
//            frame: nothing is written into the command memory, the words of a draw record come through nr_z8.h Marks by the record's
//            place in its submission. Who is ahead changes frame by frame (the walker one or two frames ahead, both starting together,
//            the translation thread first; a ring of 256 marks that the walker laps); runs are split (Writer::max_tails). The call logs
//            of APPLY and VERIFY must still be TODAY's, the walk's digest at every record whose words arrived is the model's State, the
//            copy after every frame is State (the translation thread waits for the walker at the end of a frame, as x_submit does).
//            Two fixed orders: the walker always ahead (the shares of the walk on the submitting thread) and the translation thread
//            always first (nothing arrives, nothing is taken, every chunk comes back). The marks alone: places, another record at a
//            place (found further on / not found), a submission not begun, a lapped ring, and a writer against a reader on 4 entries.
//   RANGES   (DM_NR_Z8_VIEW, section 30) the walker numbers the vertex / index ranges of a frame's answered draws; the translation
//            thread's State must bind, at every draw whose words arrived, exactly the range each number was given to (one range a
//            number, one number a range, within the frame); the numbering alone (first use, a repeat, the next submission, full).
//   VALUES   (DM_NR_Z8_VAL, section 31) the verdict of a tail draw from the translation thread's own memory: expect_diff over the
//            slot's words and State against pv_diff over the side record the walker wrote, on random templates and one changed
//            field at a time (the same code, field for field); in the THREAD runs the model takes tail draws by Reader::draw2 -
//            neither the side chunk nor the chunk's packets are read - and the logs are still TODAY's; in VERIFY the old verdict
//            finds in the chunks what the model named (k_bad 0).
//   PHONE    (NR-ZERO.md section 36: what the first phone series met and the host model had not) the descriptor sets of the model are
//            laid out by the real plan builder (nr_desc.c) the way a device with dynamic uniform buffers lays out EVERY set of the game:
//            the support block of the vertex stage, of the fragment stage and the c1 constants are dynamic buffers too - a set has
//            more dynamic offsets than ring blocks (the phone: 7.1 offsets a bind, 4.2 ring blocks a draw). Every draw the model
//            records must be describable (nr_z8.h describe(), the function the engine's publish calls), its template must give the
//            offsets of both kinds, and the verdict must compare both. The set of the phone alone (7 offsets, 4 ring blocks), a set
//            with a ring block baked in (refused), 16 / 17 offsets. And the walk's own thread with DM_NB_OWN (walker_thread()).
// The mutation check: built with -DNRZ8_MUTATE=k (k = 1..30 but 9, nr_z8.h) this test must FAIL (9 - no vertex-range compare in match() -
// must PASS: those words only give values, which the reader compares again). 27 and 30 are the engine as the phone ran it.
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
#include <chrono>
#include <deque>
#include <map>
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
static void (*g_mid_put)() = nullptr;   // marks_test: a reader in the middle of Marks::put
#define NRZ8_TEST_MID_PUT() do { if (g_mid_put) g_mid_put(); } while (0)
#include "../../../nr/nr_z8.h"
extern "C" {
#include "../../../nr/nr_desc.c"   // the real plan builder (nr_dplan_build): the model's descriptor sets are laid out by it, as the engine's
}

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
static prog_t g_prog[16]; static dpool_t g_pool[4]; static pool_t g_mem[4];
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
struct Gens { uint32_t set_gen, pipe_gen; uint64_t ring_delta; uint32_t cached = 0, warm = 0; };   // (PHONE) cached: the first shadow cascade is a cached layer this frame - its draws are dropped; warm: the frame counts in the shares
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
// nring: the game's uniform blocks (ring blocks); noff: the dynamic offsets of the set = the ring blocks + the support / c1 blocks
// the layout binds as dynamic buffers (dp: the plan the real builder makes of the two stages' binding tables).
struct Plan { unsigned nrow = 0, noff = 0, nring = 0; uint8_t rns[4], rslot[4], rsamp[4], dst[4], dsl[4]; uint16_t used = 0; uint64_t layout = 0; nr_dplan dp; };
static bool plan_of(const TS &S, Plan &p) {
    if (!S.tprog[0] || !S.tprog[4]) return false;
    const uint32_t vp = uint32_t(S.tprog[0]->serial - 100), fp = uint32_t(S.tprog[4]->serial - 100);
    static Plan memo[16][16]; static bool have[16][16];
    if (vp < 16 && fp < 16 && have[vp][fp]) { p = memo[vp][fp]; return true; }
    p.nrow = fp % 4; for (unsigned i = 0; i < p.nrow; i++) { p.rns[i] = 1; p.rslot[i] = uint8_t((i * 5 + fp) % 16); p.rsamp[i] = (fp & 8) ? 0xff : p.rslot[i]; }
    p.nring = 1 + vp % 3; for (unsigned i = 0; i < p.nring; i++) { p.dst[i] = uint8_t(i & 1); p.dsl[i] = uint8_t(i); }
    p.used = (vp & 1) ? 0x3 : 0x1; p.layout = 0x2000 + mix(vp, fp) % 4;
    // The binding tables of the two stages as the device lays them out with dynamic uniform buffers on: every uniform buffer dynamic.
    // Vertex stage: its support block first, then its ring blocks. Fragment stage: its support block (two programs in three), its ring
    // blocks, the c1 constants (one program in four), the textures. So the offsets of the fixed blocks lie before, between and behind
    // the ring blocks' in the set's order.
    nr_dbind vs[8], fs[16]; uint8_t dv[8], df[16]; unsigned nv = 0, nf = 0; uint32_t bind = 0;
    auto ubo = [&bind](nr_dbind *t, uint8_t *d, unsigned &n, uint32_t source, uint32_t slot) { t[n] = nr_dbind{bind++, NRD_T_UBO, 1, source == NRD_S_UNIFORM ? 256u : 1392u, source, slot, 0xffffffffu, 0}; d[n++] = 1; };
    ubo(vs, dv, nv, NRD_S_SUPPORT, 0);
    for (unsigned i = 0; i < p.nring; i += 2) ubo(vs, dv, nv, NRD_S_UNIFORM, i);
    if (fp % 3 != 1) ubo(fs, df, nf, NRD_S_SUPPORT, 0);
    for (unsigned i = 1; i < p.nring; i += 2) ubo(fs, df, nf, NRD_S_UNIFORM, i);
    if (fp % 4 == 2) ubo(fs, df, nf, NRD_S_C1, 0);
    for (unsigned i = 0; i < p.nrow; i++) { fs[nf] = nr_dbind{bind++, NRD_T_COMBINED, 1, 0, NRD_S_TEXTURE, p.rslot[i], p.rsamp[i] == 0xff ? 0xffffffffu : uint32_t(p.rsamp[i]), 0}; df[nf++] = 0; }
    nr_dstage st[NRD_STAGES]; memset(st, 0, sizeof st);
    st[0].b = vs; st[0].dyn = dv; st[0].n = nv; st[0].ns = nvn_stage[0]; st[0].present = 1; st[4].b = fs; st[4].dyn = df; st[4].n = nf; st[4].ns = nvn_stage[4]; st[4].present = 1;
    const int why = nr_dplan_build(&p.dp, st);
    ++checks; if (why != NRD_OK || p.dp.nubo != p.nring || p.dp.nimg != p.nrow || p.dp.nstat || p.dp.ndyn <= p.dp.nubo) { ++fails; printf("FAIL: the model's set of programs %u / %u is not the plan expected (why %d, %u ring blocks, %u offsets)\n", vp, fp, why, unsigned(p.dp.nubo), unsigned(p.dp.ndyn)); }
    p.noff = p.dp.ndyn;
    if (vp < 16 && fp < 16) { memo[vp][fp] = p; have[vp][fp] = true; }
    return true;
}
struct Answer { uint64_t did = 0, pent = 0; };
// the "NR-08 entry" and the "NR-09 entry" of the draw the State describes (every field such an entry depends on, listed here)
static Answer resolve(const TS &S, const Plan &p, uint32_t key) {
    Answer a; uint64_t h = mix(S.tprog[0]->serial, S.tprog[4]->serial);
    h = mix(h, uint64_t(uint32_t(S.viewport.width)) << 32 | uint32_t(S.viewport.height)); h = mix(h, uint64_t(uintptr_t(S.texture_pool.pool))); h = mix(h, uint64_t(S.texture_pool.off)); h = mix(h, uint64_t(S.texture_pool.n));
    h = mix(h, uint64_t(uintptr_t(S.sampler_pool.pool))); h = mix(h, uint64_t(S.sampler_pool.off));
    for (unsigned i = 0; i < p.nrow; i++) { h = mix(h, S.tex[p.rns[i]][p.rslot[i]]); h = mix(h, p.rsamp[i] != 0xff ? S.samp[p.rns[i]][p.rsamp[i]] : 0); }
    for (unsigned i = 0; i < p.nring; i++) { const uint64_t z = S.ub[p.dst[i]][p.dsl[i]].size; h = mix(h, z < 256 ? z : 256); }
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
    for (unsigned i = 0; i < p.dp.nrow; i++) { const nr_drow &r = p.dp.row[i];   // the set's dynamic offsets, in the set's order (dyn_at)
        if (r.kind == NRD_K_RING_DYN) { const uint64_t ad = S.ub[r.ns][r.slot].address; if (!ad || ad < g.ring_delta || ad - g.ring_delta > 0xffffffffull) return false; off[r.dyn_at] = uint32_t(ad - g.ring_delta); }   // a ring block: of the moment
        else if (r.kind == NRD_K_FIXED_DYN) off[r.dyn_at] = 256u * uint32_t(1 + mix(a.did, r.binding) % 5); }   // a support / c1 block: the entry's offset (as nrdesc::Ent::off)
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
// BENCH (NR-ZERO.md section 39; ./nr_z8_test bench): parts of the walk left out to see what each costs - 1 the test's own checks
// (digest, fresh fix id), 2 the template (look and all after it), 4 the packets, 8 the ranges' numbers, 16 the fix id, 32 everything of
// a draw record after its binds.
static unsigned g_bench = 0;
struct Walk {
    Sh S; nrz8::Fix fix; nrz8::Walker<Walk> W; nrz5r::Writer wr; bool bad = false; uint8_t stamp = 0; uint64_t draws = 0, values = 0, miss[nrz8::M_ALL] = {}, fix_stale = 0;
    std::vector<uint64_t> *dig = nullptr; nrz8::Marks *marks = nullptr;   // marks: the walk on its own thread - the words go there, not into the record
    nrz8::Ranges vr; uint64_t vw = 0;
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
        ++draws; if (dig && !(g_bench & 1)) dig->push_back(digest(S));
        if (g_bench & 32) { wr.no_values(); return; }
        nrd_tail t; const bool nr = nrd_tail_get(rec, n, &h, &t) != 0; uint32_t mark = 0; int32_t slot1 = 0; uint64_t ca = 0; vw = 0; int why = nrz8::M_NOKEY;
        if (!top || !nr || !t.key || (h.flags & (NBD_F_REN | NBD_F_COND)) || h.count <= 0) { wr.no_values(); if (!top) why = nrz8::M_CALLED; }
        else {
            nrz5_site site; nrz5_site_make(&site, t.key, h.draw == NBD_DRAW_IDX ? NRZ5S_IDX : NRZ5S_ARRAYS, h.prim, h.itype, h.count, h.base_vertex, h.base_instance, h.instances, h.first, h.ibuf, S.vb[0].address);
            const uint32_t fixid = (g_bench & 16) ? 1u : fix.get(S, S.sprog); const uint32_t bk = g_tab.slot_of(&site, fixid); if (!(g_bench & 17) && fixid != fix.fresh(S, S.sprog)) ++fix_stale;
            uint64_t w[nrz8::MAX_WORDS]; nrz5_vals v; uint32_t off[nrz8::MAX_DYN]; ca = fixid;
            nrz8::Look L; if (!(g_bench & 2)) L = nrz8::look(g_tab, bk, site, fixid, h, Acc{S}, w, &v, off); slot1 = int32_t(L.slot + 1); ++miss[L.m]; why = L.m;   // (the engine's walk: xbg_nr_z8.inc Hnd::nb_draw)
            if (!nrz8::answered(L.m)) wr.no_values();
            else { ca |= uint64_t(L.ver) << 32;
                if (L.m == nrz8::M_OK && !(g_bench & 4)) { ++values; mark = wr.draw(v, [](nbmtq::Chunk *ch, nrz5_cache *mm, const nrz5_vals *vv) { nbmt::Build bb{ch}; nbmt::Z5Sink sink{bb}; return nrz5_emit(sink, mm, vv); }); }
                else wr.no_values();
                if (!(g_bench & 8)) vw = nrz8::ranges_word(vr, v.used, [this](uint32_t i, uint64_t *a, uint64_t *z) { *a = S.vb[i].address; *z = S.vb[i].size; return i < 16; }, h.draw == NBD_DRAW_IDX, h.ibuf, uint64_t(uint32_t(h.count)) << h.itype); }
        }
        if (marks) { nrz8::MarkVal mv; mv.ca = (h.flags & NBD_F_COND) ? 0 : ca; mv.mark = mark; mv.slot1 = slot1; mv.aux = (g_bench & 1) ? 1 : digest(S) | 1; mv.vw = vw; mv.pk = mark ? uint64_t(wr.last_bytes) | uint64_t(wr.last_cmds) << 32 : 0; mv.why = uint8_t(why); marks->put(rec, mv); return; }
        rec[offsetof(nbd_hdr, _pad0)] = stamp; memcpy(rec + offsetof(nbd_hdr, _pad1), &mark, 4); memcpy(rec + offsetof(nbd_hdr, _pad2), &slot1, 4);
        if (!(h.flags & NBD_F_COND)) memcpy(rec + offsetof(nbd_hdr, cond_addr), &ca, 8);
    }
    uint64_t walk_ns = 0, warm_ns = 0, warm_draws = 0, warm_records = 0;
    void submit(const Frame &f, uint64_t sub) { const uint64_t t0 = nbmtq::now_ns(), d0 = draws, r0 = W.records; submit_(f, sub); const uint64_t dt = nbmtq::now_ns() - t0; walk_ns += dt;
        if (f.g.warm) { warm_ns += dt; warm_draws += draws - d0; warm_records += W.records - r0; } }
    void submit_(const Frame &f, uint64_t sub) { stamp = nrz8::stamp_of(sub); vr.begin(); uint64_t budget = 0; for (uint64_t hd : f.handles) { if (!W.walk(reinterpret_cast<const uint8_t *>(uintptr_t(hd)), 0, budget, true)) bad = true; wr.end_run(); } }
};

// ---------------- the translation thread ----------------
struct Priv { uint32_t ver = 0; Answer a; uint32_t set_gen = 0, pipe_gen = 0; const prog_t *sh[2] = {}; };   // sh: the programs of the vertex / fragment stage the answer was resolved for (after the prologue)
struct Tot { uint64_t described = 0, not_desc = 0, draws = 0, recorded = 0, valid = 0, answered = 0, stale = 0, a_cmp = 0, a_bad = 0, dig_cmp = 0, published = 0, m_got = 0, m_not = 0, m_nobase = 0, m_desync = 0, m_resync = 0, waits = 0, splits = 0, walk_ns = 0, v_ask = 0, v_rep = 0; nrz5r::RStats r; uint64_t w_draws = 0, w_values = 0, w_tails = 0, w_fix_stale = 0, w_miss[nrz8::M_ALL] = {};
             uint64_t warm_ns = 0, warm_draws = 0, warm_records = 0, lane = 0, lane_ans = 0, lane_pro[3] = {}, lane_pro_ans[3] = {}, dropped = 0, warm = 0; } tot;   // (PHONE) lane: drawn draws whose words arrived, after the warm-up frames; lane_ans: of them answered; by the prologue's kind
// [PHONE, NR-ZERO.md section 39] The prologue of the translation thread's draw() (xbg_game.cpp) in the model: 0 none; 1 a twin - only the
// fragment program is swapped (NR-MAT / NR-SH-SURF: depth-only passes, a material class, a word of a uniform block now and then says no);
// 2 the prologue changes State (LIGHTFUSE, the pre-pass override: here rasterizer discard). The walk's copy is the State BEFORE it.
static bool g_phone = false;
static int pro_ok(int pro) { return nrz8::prologue_ok(pro, 1); }   // the engine's rule (xbg_game.cpp z8g / z8tw, DM_NR_Z8_TWIN=1)
static bool same_prog(const prog_t *const a[2], const TS &S) { const prog_t *const b[2] = {S.tprog[0], S.tprog[4]}; return nrz8::same_shaders(a, b, 2); }   // (the engine: same_shaders over the five stages, the fragment last)
static nrz5r::Reader *g_R = nullptr;
static void pre_hook() { if (g_R && g_R->holding) g_R->flush(nrz5r::CUT_WRITE); }
static uint64_t sig() { const nbmtq::Stats &q = nbmt::q.st; return q.cmds * 8 + q.sync[nbmtq::NBMT_SY_PNEXT] + q.sync[nbmtq::NBMT_SY_ALLOC] + q.sync[nbmtq::NBMT_SY_CALLER]; }
static bool is_draw(uint16_t op) { return op == nbmt::OP_Draw || op == nbmt::OP_DrawIndexed; }
struct Xl {   // one run of the model: mode 0 today, 1 apply, 2 verify
    TS S; nrz8::Def defs[NBS_KINDS][NBS_MAX_IDS] = {}; int mode = 0; nrz5r::Reader *R = nullptr; std::vector<Priv> priv; std::vector<uint32_t> used; uint32_t uclock = 0; Gens g{}; uint8_t stamp = 0;
    VkCommandBuffer c{}; BindCache bc; const std::vector<uint64_t> *dig = nullptr; size_t dig_at = 0; std::string *outcome = nullptr;
    const nrz8::Marks *marks = nullptr; nrz8::MarkReader mr;   // the walk on its own thread: the record's words by its place
    struct RKey { uint64_t a, z; uint32_t tag; bool operator<(const RKey &o) const { return a != o.a ? a < o.a : z != o.z ? z < o.z : tag < o.tag; } bool operator==(const RKey &o) const { return a == o.a && z == o.z && tag == o.tag; } };
    std::map<uint32_t, RKey> id2k; std::map<RKey, uint32_t> k2id; uint64_t cur_vw = 0;   // the ranges of the frame in hand, as their numbers arrive
    uint64_t cur_pk = 0; int kmode = 0;   // the packets of the tail draw in hand; 1: tail draws are taken by Reader::draw2, 2: compared (Reader::chk)
    // DM_NR_Z8_VAL: the verdict from this thread's own memory (the slot's words, State)
    bool known(const nbd_hdr &h, uint32_t ver, const nrz5_vals &v, nrz5r::Reader::Known *k) {
        if (!cur_pk || !ver || h._pad2 <= 0 || priv[h._pad2 - 1].ver != ver) return false;
        const nrz8::Slot &x = g_tab.s[h._pad2 - 1];
        struct A { const TS &S; bool ub(unsigned st, unsigned sl, uint64_t *a, uint64_t *z) const { if (st >= 6 || sl >= 16) return false; *a = S.ub[st][sl].address; *z = S.ub[st][sl].size; return true; } };
        k->d = nrz8::expect_diff([&x](unsigned j) { return x.w[j].load(std::memory_order_relaxed); }, &v, h.count, h.instances, h.first, h.base_vertex, h.base_instance, A{S});
        k->bytes = uint32_t(cur_pk); k->n = uint32_t(cur_pk >> 32); return true; }
    void range(uint32_t id, const RKey &k) { ++tot.v_ask; auto a = id2k.find(id); auto b = k2id.find(k);
        if (a == id2k.end() && b == k2id.end()) { id2k[id] = k; k2id[k] = id; return; }
        ++tot.v_rep; CHECK(a != id2k.end() && b != k2id.end() && a->second == k && b->second == id, "ranges: number %u names one range of the frame and that range has one number", id); }
    void other(uint32_t op, const uint8_t *p) { uint32_t z; memcpy(&z, p, 4);
        if (op == OP_T_SILENT) { bc = BindCache{}; return; }
        if (op == OP_T_PASS) { nbmt::CmdEndRenderPass(c); VkRenderPassBeginInfo b{}; b.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; b.renderPass = hnd<VkRenderPass>(z & 7); b.renderArea.extent = {640, 360}; nbmt::CmdBeginRenderPass(c, &b, VK_SUBPASS_CONTENTS_INLINE); bc = BindCache{}; }
        else if (op == OP_T_OTHER) nbmt::CmdFillBuffer(c, hnd<VkBuffer>(0x77), z & 0xff0, 16, z); }
    void publish(const nbd_hdr &h, uint32_t key, const Plan &p, const Answer &a, const nrz5_vals &v, int pro) {
        if (mode == 0 || h._pad2 <= 0 || uint32_t(h._pad2 - 1) >= g_tab.n) return;
        nrz8::Desc d; nrz5_site_make(&d.site, key, h.draw == NBD_DRAW_IDX ? NRZ5S_IDX : NRZ5S_ARRAYS, h.prim, h.itype, h.count, h.base_vertex, h.base_instance, h.instances, h.first, h.ibuf, S.vb[0].address);
        d.fixid = uint32_t(h.cond_addr); d.first_index = 0; d.k = 0;
        const uint32_t hs = uint32_t(h._pad2 - 1);   // (the engine's publish: the way of the bucket - nr_z8.h pick - and the template that says no)
        auto put = [&](const uint64_t *w, unsigned nw) -> Priv & { const uint32_t sl = g_tab.pick(hs, w, used.data()); used[sl] = ++uclock;
            Priv &x = priv[sl]; const uint32_t nv = g_tab.put(sl, w, nw); if (nv != x.ver) { x = Priv{}; x.ver = nv; ++tot.published; } return x; };
        if (!pro_ok(pro)) { uint64_t z[nrz8::HEAD_WORDS]; put(z, nrz8::none_of(d.site, d.fixid, z)); return; }
        // the description is nr_z8.h's - the function the engine's publish calls - over the plan of the set and this thread's State
        uint32_t copy[NRD_MAX_UBO]; for (unsigned u = 0; u < p.dp.nubo; u++) { const nr_drow &r = p.dp.row[p.dp.ubo_row[u]]; const uint64_t z = S.ub[r.ns][r.slot].size; copy[u] = uint32_t(z < 256 ? z : 256); }
        struct A { const TS &S;
            bool ub(unsigned st, unsigned sl, uint64_t *x, uint64_t *z) const { if (st >= 6 || sl >= 16) return false; *x = S.ub[st][sl].address; *z = S.ub[st][sl].size; return true; }
            bool vb(unsigned i, uint64_t *x, uint64_t *z) const { if (i >= 16) return false; *x = S.vb[i].address; *z = S.vb[i].size; return true; }
            uint64_t tex(unsigned ns, unsigned sl) const { return ns < 6 && sl < 128 ? S.tex[ns][sl] : ~0ull; } uint64_t samp(unsigned ns, unsigned sl) const { return ns < 6 && sl < 128 ? S.samp[ns][sl] : ~0ull; } };
        const int why = nrz8::describe(p.dp, copy, v, A{S}, d); ++tot.described;
        CHECK(why == nrz8::P_OK, "phone: a recorded draw whose set binds %u ring blocks and %u support / c1 blocks as dynamic buffers is a template (not describable, reason %d - on the phone: \"a static uniform block\", every draw)", unsigned(p.dp.nubo), unsigned(p.dp.ndyn - p.dp.nubo), why);
        if (why) { ++tot.not_desc; return; }
        uint64_t w[nrz8::MAX_WORDS]; const unsigned nw = nrz8::pack(d, w); if (!nw) return;
        Priv &pv = put(w, nw); pv.a = a; pv.set_gen = g.set_gen; pv.pipe_gen = g.pipe_gen; pv.sh[0] = S.tprog[0]; pv.sh[1] = S.tprog[4];
    }
    void draw(const uint8_t *rec, size_t n) {
        nbd_hdr h; const uint8_t *b = nullptr; CHECK(nbd_view(rec, n, &h, &b) == 0, "record"); unsigned ts = 0, bs = 0; nbd_apply(&h, b, S.tex, S.samp, S.img, reinterpret_cast<nbd_range (*)[16]>(S.ub), &ts, &bs);
        if (dig && dig_at < dig->size()) { ++tot.dig_cmp; CHECK((*dig)[dig_at] == digest(S), "the walk's copy is State at draw record %zu", dig_at); ++dig_at; }
        nrd_tail t{}; nrd_tail_get(rec, n, &h, &t); const uint32_t key = t.key; ++tot.draws;
        bool mine = mode && h._pad0 == stamp; cur_vw = 0; cur_pk = 0;
        if (marks) {   // the record itself carries nothing (whatever an earlier run left in it is not read)
            h._pad0 = 0; h._pad1 = 0; h._pad2 = 0; if (!(h.flags & NBD_F_COND)) h.cond_addr = 0;
            nrz8::MarkVal mv; mine = mode && mr.next(*marks, rec, &mv);
            if (mine) { h._pad0 = stamp; h._pad1 = mv.mark; h._pad2 = mv.slot1; if (!(h.flags & NBD_F_COND)) h.cond_addr = mv.ca; cur_vw = mv.vw; cur_pk = mv.pk;
                ++tot.dig_cmp; CHECK(mv.aux == (digest(S) | 1), "thread: the walk's copy is State at the draw record whose words arrived (record %u of its frame)", mr.ord - 1); } }
        const uint32_t mw = mine ? h._pad1 : 0u, ver = mine && !(h.flags & NBD_F_COND) ? uint32_t(h.cond_addr >> 32) : 0u; if (!mine) h._pad2 = 0;
        Plan p; bool dropped = !plan_of(S, p) || mix(key, S.tprog[4]->serial) % 37 == 0;   // the prologue of draw() drops it (a function of State)
        // [PHONE] the prologue: the shadow cache drops the draws of a cached cascade; a twin swaps the fragment program; LIGHTFUSE changes State
        struct Undo { TS &S; const prog_t *fp; uint32_t rd; ~Undo() { S.tprog[4] = fp; S.rast_discard = rd; } } undo{S, S.tprog[4], S.rast_discard};
        int pro = 0;
        if (g_phone && !dropped) {
            if (S.rt.n == 0 && S.rt.depth == reinterpret_cast<const tex_t *>(uintptr_t(0x9800)) && g.cached) dropped = true;
            else if (S.rt.n == 0 && (S.tprog[4]->serial == 106 || S.tprog[4]->serial == 107) && (S.ub[0][0].address >> 8) % 64 != 0) { S.tprog[4] = &g_prog[S.tprog[4]->serial - 100 + 6]; pro = 1; dropped = !plan_of(S, p); }
            else if (S.rt.n == 1 && S.tprog[4]->serial == 111 && key == 170 && g.cached) { S.rast_discard = 1; pro = 2; } }   // (LIGHTFUSE fuses in some frames: the same words of the walk, another State)
        if (g_phone && dropped) ++tot.dropped;
        if (cur_vw && plan_of(S, p)) {   // the numbers the walker gave this draw's ranges name the ranges State binds (the lane's tail would ask for exactly these)
            unsigned k = 0; for (uint32_t m = p.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); range(uint32_t(cur_vw >> (16 * k++)) & 0xffffu, RKey{S.vb[i].address, S.vb[i].size, 1}); }
            CHECK(k <= nrz8::VW_MAX_VB && (cur_vw >> (16 * k) & ((1ull << (48 - 16 * k)) - 1)) == 0, "ranges: a number for every used binding and no other");
            if (h.draw == NBD_DRAW_IDX) range(uint32_t(cur_vw >> 48), RKey{h.ibuf, uint64_t(uint32_t(h.count)) << h.itype, 2}); else CHECK((cur_vw >> 48) == 0, "ranges: no index range for a draw without indices"); }
        auto miss = [&] { if (!mode) return; if (R->holding) { if (mode == 2) R->verify_miss(); else R->flush(nrz5r::CUT_RECORD); } else R->abandon(mw); };
        if (dropped) { miss(); if (outcome) outcome->push_back('D'); return; }
        Answer a; bool answered = false;
        if (mode == 1 && ver && h._pad2 > 0 && priv[h._pad2 - 1].ver == ver) { const Priv &pv = priv[h._pad2 - 1];
            if (pv.set_gen == g.set_gen && pv.pipe_gen == g.pipe_gen && pro_ok(pro) && same_prog(pv.sh, S)) { a = pv.a; answered = true; ++tot.answered; used[uint32_t(h._pad2 - 1)] = ++uclock; } else ++tot.stale; }   // (State is NOT asked: the answer must be right by itself; the programs after the prologue are the answer's)
        if (!answered) a = resolve(S, p, key);
        if (g_phone && mine && g.warm) { ++tot.lane; ++tot.lane_pro[pro]; const bool an = mode == 1 ? answered : ver && h._pad2 > 0 && priv[h._pad2 - 1].ver == ver && pro_ok(pro) && same_prog(priv[h._pad2 - 1].sh, S); if (an) { ++tot.lane_ans; ++tot.lane_pro_ans[pro]; } }
        if (mode == 2 && ver && h._pad2 > 0 && priv[h._pad2 - 1].ver == ver && priv[h._pad2 - 1].set_gen == g.set_gen && priv[h._pad2 - 1].pipe_gen == g.pipe_gen && pro_ok(pro) && same_prog(priv[h._pad2 - 1].sh, S)) { ++tot.a_cmp;
            if (priv[h._pad2 - 1].a.did != a.did || priv[h._pad2 - 1].a.pent != a.pent) { ++tot.a_bad; CHECK(false, "verify: the walk's answer is the model's (key %u)", key); } }
        nrz5_vals v; uint32_t off[nrz8::MAX_DYN];
        if (!values(S, p, a, g, h, v, off)) { miss(); if (outcome) outcome->push_back('D'); return; }
        const bool foreign = key % 41 == 0;
        if (!mode || nbmt::cur != c) { if (mode) R->abandon(mw); ref_tail(c, bc, v); ++tot.recorded; if (mode) publish(h, key, p, a, v, pro); if (foreign) { const VkPipeline fp = hnd<VkPipeline>(0x1999); nbmt::CmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, fp); bc.pipe = fp; } if (outcome) outcome->push_back('R'); return; }
        auto holds = [&](const nrz5_cache &sh, const nrz5_vals &vv) { const nrz5_cache real = conv(bc); return nrz5r::cache_holds(&sh, &real, &vv); };
        char o = 'R';
        nrz5r::Reader::Known kn{}; const nrz5r::Reader::Known *kp = kmode && mw && known(h, ver, v, &kn) ? &kn : nullptr; R->chk = kmode == 2 || mode == 2 ? kp : nullptr;
        if (mode == 2) {
            const bool would = R->verify_begin(mw, v, holds, is_draw, sig()); if (would) { ++tot.valid; o = 'V'; }
            nbmtq::Chunk *c0 = nbmt::q.cur; const uint32_t u0 = c0 ? c0->used : 0; const uint64_t ch0 = nbmt::q.st.chunks;
            ref_tail(c, bc, v); ++tot.recorded;
            nbmtq::Chunk *cc = c0 ? c0 : nbmt::q.cur; const uint32_t from = c0 ? u0 : 0; const bool readable = cc && cc == nbmt::q.cur && ch0 == nbmt::q.st.chunks;
            R->verify_end(readable ? cc->data() + from : nullptr, readable ? cc->used - from : 0, [](const uint8_t *x, uint32_t na, const uint8_t *y, uint32_t nb) { uint32_t at = 0; return nbmt::same_stream(x, na, y, nb, &at); }, sig());
            publish(h, key, p, a, v, pro);
        } else if ((kmode == 1 ? R->draw2(mw, v, kp, holds, is_draw) : R->draw(mw, v, holds, is_draw)) == nrz5r::Reader::VALID) { leave(bc, v); ++tot.valid; o = 'V'; }
        else { ref_tail(c, bc, v); ++tot.recorded; publish(h, key, p, a, v, pro); }
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
// wk: 0 the walk before the frame is executed, its words in the records (the walk on the submitting thread); 1 the walk on its own
// thread, who is ahead changes frame by frame; 2 the same, the walker always a frame ahead; 3 the same, the translation thread first.
static std::vector<std::vector<uint8_t>> run(const std::vector<Frame> &prog, int mode, uint64_t seed, std::string *outcome = nullptr, bool threads = true, int wk = 0) {
    std::vector<std::vector<uint8_t>> logs(prog.size()); tot = Tot{}; if (!mode) wk = 0;
    nrz8::Marks marks; std::mutex jm; std::condition_variable jc; std::deque<size_t> jobs; bool jdone = false; std::map<size_t, std::unique_ptr<Sh>> ends; std::thread wt; size_t posted = 0;
    auto X = std::make_unique<Xl>(); X->mode = mode; X->outcome = outcome; nrz5r::Reader R; R.q = &nbmt::q; R.ring = &g_ring; X->R = &R; Rng r{seed * 31 + 3};
    if (mode) { CHECK(g_tab.init(8192), "table"); X->priv.resize(g_tab.n); X->used.assign(g_tab.n, 0); g_pre = mode == 1 ? pre_hook : nullptr; g_R = mode == 1 ? &R : nullptr; }
    auto W = std::make_unique<Walk>(); Handoff h; std::thread gt; if (g_bench & 64) W->wr.side = false;
    auto walk_one = [&](size_t f) { Item it; it.f = f; if (mode) { W->dig = &it.dig; W->submit(prog[f], f + 1); W->dig = nullptr; it.end.reset(new Sh(W->S)); } return it; };
    if (mode) { CHECK(W->fix.init() && W->W.init(), "walk"); W->W.h = W.get(); W->wr.q = &nbmt::q; W->wr.ring = &g_ring; W->wr.min_tails = 1 + uint32_t(seed % 3); }
    if (wk) {
        CHECK(marks.init(wk == 1 && seed % 2 ? 256 : 65536), "marks"); W->marks = &marks; X->marks = &marks; X->kmode = mode == 2 ? 2 : 1 + int(seed / 7 % 2);   // (the verdict by draw2 / the old verdict compared with it)
        W->wr.side = !(mode == 1 && X->kmode == 1);   // (the engine's init: no values records for a reader that names every verdict itself)
        wt = std::thread([&] { Rng wr{seed * 13 + 5};
            for (;;) { size_t f; { std::unique_lock<std::mutex> l(jm); jc.wait(l, [&] { return !jobs.empty() || jdone; }); if (jobs.empty()) return; f = jobs.front(); jobs.pop_front(); }
                marks.begin(f + 1); W->wr.max_tails = wr.upto(3) ? 1 + wr.upto(6) : 0; W->submit(prog[f], f + 1);
                { std::lock_guard<std::mutex> l(jm); ends[f].reset(new Sh(W->S)); } marks.end(); } });
    }
    auto post = [&](size_t f) { while (posted <= f && posted < prog.size()) { { std::lock_guard<std::mutex> l(jm); jobs.push_back(posted); } jc.notify_one(); ++posted; } };
    auto walked = [&](size_t f) { while (marks.done.load(std::memory_order_acquire) < f + 1) std::this_thread::yield(); };
    if (mode && threads && !wk) gt = std::thread([&] { for (size_t f = 0; f < prog.size(); f++) { Item it = walk_one(f); std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return h.q.size() < 3; }); h.q.push_back(std::move(it)); h.cv.notify_all(); }
        { std::lock_guard<std::mutex> l(h.mu); h.done = true; } h.cv.notify_all(); });
    for (size_t f = 0; f < prog.size(); f++) {
        Item it;
        if (wk) { const uint32_t pol = wk == 1 ? r.upto(4) : wk == 2 ? 1u : 9u;   // who is ahead in this frame
            if (pol < 4) post(f); if (pol == 2) post(f + 1); if (pol == 1) walked(f); if (pol == 3) std::this_thread::sleep_for(std::chrono::microseconds(30));
            X->mr.begin(f + 1); X->id2k.clear(); X->k2id.clear(); it.f = f; }
        else if (mode && threads) { std::unique_lock<std::mutex> l(h.mu); h.cv.wait(l, [&] { return !h.q.empty() || h.done; }); CHECK(!h.q.empty(), "a frame for every walk"); if (h.q.empty()) break; it = std::move(h.q.front()); h.q.pop_front(); h.cv.notify_all(); }
        else it = walk_one(f);
        const bool direct = mode == 1 && r.upto(16) == 2;   // this recording goes by direct calls: nothing written ahead is used
        Log lg; X->c = reinterpret_cast<VkCommandBuffer>(&lg); X->bc = BindCache{}; X->g = prog[f].g; X->stamp = nrz8::stamp_of(f + 1); X->dig = mode ? &it.dig : nullptr; X->dig_at = 0;
        nbmt::report_ns = nbmtq::now_ns(); nbmt::begin(X->c, !direct, false);
        for (uint64_t hd : prog[f].handles) { X->exec(reinterpret_cast<const uint8_t *>(uintptr_t(hd)), 0); if (mode) { if (R.holding) { if (mode == 2) R.verify_miss(); else R.flush(nrz5r::CUT_RECORD); } } }
        nbmt::end(X->c); logs[f] = std::move(lg.b);
        CHECK(!R.holding, "a run does not outlive its command buffer");
        if (wk) {   // the end of the submission: the walker must have left it (x_submit waits here); then its copy is State
            post(f); if (marks.done.load(std::memory_order_acquire) < f + 1) { ++tot.waits; walked(f); }
            std::lock_guard<std::mutex> l(jm); it.end = std::move(ends[f]); ends.erase(f); CHECK(it.end != nullptr, "thread: the walker left frame %zu", f); if (!it.end) break; }
        if (mode) { CHECK(X->dig_at == it.dig.size(), "every draw record the walk saw was executed (%zu of %zu)", X->dig_at, it.dig.size());
            const char *d = state_diff(*it.end, X->S); CHECK(!d, "frame %zu: the walk's copy differs from State in %s", f, d ? d : "");
            for (unsigned st = 0; st < 6; st++) CHECK(it.end->sprog[st] == (X->S.tprog[st] ? X->S.tprog[st]->serial : 0), "frame %zu: program word of stage %u", f, st); }
    }
    if (gt.joinable()) gt.join();
    if (wk) { { std::lock_guard<std::mutex> l(jm); jdone = true; } jc.notify_all(); wt.join(); tot.m_got = X->mr.got; tot.m_not = X->mr.not_there; tot.m_nobase = X->mr.no_base; tot.m_desync = X->mr.desync; tot.m_resync = X->mr.resync;
        tot.splits = W->wr.splits; CHECK(X->mr.desync == 0 && X->mr.resync == 0, "thread: both threads count the draw records alike (%llu / %llu)", (unsigned long long)X->mr.desync, (unsigned long long)X->mr.resync); marks.destroy(); }
    if (mode) { R.drop(); g_R = nullptr; g_pre = nullptr; tot.r = R.st; tot.walk_ns = W->walk_ns; tot.warm_ns = W->warm_ns; tot.warm_draws = W->warm_draws; tot.warm_records = W->warm_records; tot.w_draws = W->draws; tot.w_values = W->values; tot.w_tails = W->wr.st.tails; tot.w_fix_stale = W->fix_stale; memcpy(tot.w_miss, W->miss, sizeof tot.w_miss);
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
// ---------------- the marks: a record's words by its place in the submission ----------------
static uint8_t g_recs[64];
static nrz8::MarkVal mval(uint64_t sub, uint32_t k) { nrz8::MarkVal v; v.ca = mix(sub, k) | 1; v.mark = uint32_t(mix(k, sub)); v.slot1 = int32_t(mix(sub * 3, k) & 0x7fff); v.aux = mix(sub * 7, k); return v; }
static bool msame(const nrz8::MarkVal &a, const nrz8::MarkVal &b) { return a.ca == b.ca && a.mark == b.mark && a.slot1 == b.slot1 && a.aux == b.aux; }
static nrz8::Marks *g_mp_m = nullptr; static bool g_mp_bad = false; static int g_mp_asked = 0;
static void marks_test() {
    { // a reader in the middle of a put that laps the ring onto an entry of the SAME record (a handle executed twice in a submission):
      // the entry of place 4 must not be readable before its words are written - it still holds the words of place 0
      nrz8::Marks m; CHECK(m.init(4), "marks"); m.begin(5); for (uint32_t k = 0; k < 4; k++) m.put(g_recs, mval(5, k));
      g_mp_m = &m; g_mp_bad = false; g_mp_asked = 0;
      g_mid_put = [] { nrz8::MarkReader q; q.begin(5); q.ord = 4; nrz8::MarkVal o; ++g_mp_asked; if (q.next(*g_mp_m, g_recs, &o) && !msame(o, mval(5, 4))) g_mp_bad = true;
                       nrz8::MarkReader q0; q0.begin(5); if (q0.next(*g_mp_m, g_recs, &o)) g_mp_bad = true; };
      m.put(g_recs, mval(5, 4)); g_mid_put = nullptr;
      CHECK(g_mp_asked == 1 && !g_mp_bad, "marks: an entry being written is not there - neither as the place it held nor as the place it will hold");
      nrz8::MarkReader q; q.begin(5); q.ord = 4; nrz8::MarkVal o; CHECK(q.next(m, g_recs, &o) && msame(o, mval(5, 4)), "marks: after the put it is the new place's"); m.destroy(); }
    { nrz8::Marks m; CHECK(m.init(64), "marks"); nrz8::MarkReader rd; nrz8::MarkVal o;
      rd.begin(5); CHECK(!rd.next(m, g_recs, &o) && rd.no_base == 1, "marks: a submission whose walk has not begun has no words");
      m.begin(5); for (uint32_t k = 0; k < 10; k++) m.put(g_recs + k, mval(5, k)); m.end(); CHECK(m.done.load() == 5, "marks: done");
      rd.begin(5); for (uint32_t k = 0; k < 3; k++) CHECK(rd.next(m, g_recs + k, &o) && msame(o, mval(5, k)), "marks: record %u has its words", k);
      CHECK(rd.next(m, g_recs + 4, &o) && msame(o, mval(5, 4)) && rd.resync == 1 && rd.ord == 5, "marks: a record the reader did not execute is passed over");
      CHECK(!rd.next(m, g_recs + 40, &o) && rd.desync == 1, "marks: a record the walk never saw has no words");
      CHECK(rd.next(m, g_recs + 6, &o) && msame(o, mval(5, 6)), "marks: the place after it stands");
      for (uint32_t k = 7; k < 10; k++) CHECK(rd.next(m, g_recs + k, &o), "marks: record %u", k);
      CHECK(!rd.next(m, g_recs + 10, &o) && rd.not_there == 1, "marks: a record the walk has not reached has no words");
      // the same addresses in the next submission: its entries are its own
      m.begin(6); rd.begin(6); CHECK(!rd.next(m, g_recs, &o), "marks: nothing of submission 6 is written yet");
      m.put(g_recs, mval(6, 0)); rd.begin(6); CHECK(rd.next(m, g_recs, &o) && msame(o, mval(6, 0)), "marks: submission 6, record 0");
      rd.begin(5); CHECK(rd.next(m, g_recs, &o) && msame(o, mval(5, 0)), "marks: submission 5 is still there (64 entries)");
      rd.begin(7); CHECK(!rd.next(m, g_recs, &o), "marks: submission 7 not begun"); m.destroy(); }
    { // a ring of 8 lapped: submission 5 wrote places 0..7 for these addresses, submission 6 begins at the same entries
      nrz8::Marks m; CHECK(m.init(8), "marks"); nrz8::MarkReader rd; nrz8::MarkVal o;
      m.begin(5); for (uint32_t k = 0; k < 8; k++) m.put(g_recs + k, mval(5, k)); m.end();
      m.begin(6); rd.begin(6); for (uint32_t k = 0; k < 8; k++) CHECK(!rd.next(m, g_recs + k, &o), "marks: an entry of submission 5 is not record %u of submission 6", k);
      for (uint32_t k = 0; k < 20; k++) m.put(g_recs + k, mval(6, k)); m.end();
      rd.begin(6); for (uint32_t k = 0; k < 12; k++) CHECK(!rd.next(m, g_recs + k, &o), "marks: lapped entry %u", k);
      for (uint32_t k = 12; k < 20; k++) CHECK(rd.next(m, g_recs + k, &o) && msame(o, mval(6, k)), "marks: entry %u after the lap", k);
      nrz8::MarkReader r2; r2.begin(6); CHECK(r2.next(m, g_recs + 12, &o) == false && r2.ord == 1, "marks: place 0 was lapped (its entry is place 16's)"); m.destroy(); }
    { // a writer that laps a ring of 4 against a reader: words are whole or not there
      nrz8::Marks m; CHECK(m.init(4), "marks"); std::atomic<bool> stop{false}; std::atomic<uint64_t> cur{0}; uint64_t good = 0, torn = 0;
      std::thread wr([&] { for (uint64_t s = 1; !stop.load(std::memory_order_relaxed); s++) { m.begin(s); cur.store(s, std::memory_order_release); for (uint32_t k = 0; k < 6; k++) m.put(g_recs + (s + k) % 64, mval(s, k)); m.end(); } });
      nrz8::MarkReader rd; nrz8::MarkVal o; const auto t0 = std::chrono::steady_clock::now();
      while (good < 200000 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) { const uint64_t s = cur.load(std::memory_order_acquire); if (!s) continue; rd.begin(s);
          for (uint32_t k = 0; k < 6; k++) if (rd.next(m, g_recs + (s + k) % 64, &o)) { if (msame(o, mval(s, k))) ++good; else ++torn; } }
      stop.store(true); wr.join();
      CHECK(torn == 0 && good > 0, "marks: no torn words (%llu torn, %llu whole)", (unsigned long long)torn, (unsigned long long)good); CHECK(rd.desync == 0 && rd.resync == 0, "marks: a lapped entry is 'not there', never another record");
      printf("marks: %llu whole reads against a lapping writer, torn %llu\n", (unsigned long long)good, (unsigned long long)torn); m.destroy(); }
}
// ---------------- the verdict without the walker's chunks: expect_diff against pv_diff over the side record ----------------
static void values_test() {
    Rng r{23}; long same = 0, differ = 0;
    for (int it = 0; it < 200000; it++) {
        nrz8::Desc d; nrz5_vals v; memset(&v, 0, sizeof v); uint32_t voff[nrz8::MAX_DYN];
        const bool indexed = r.upto(4) != 0; const int32_t count = int32_t(3 + r.upto(900)), instances = indexed ? int32_t(1 + r.upto(2)) : 0, first = indexed ? 0 : int32_t(r.upto(60)), bv = int32_t(r.upto(50)), bi = indexed ? int32_t(r.upto(3)) : 0;
        nrz5_site_make(&d.site, 1 + r.upto(40), indexed ? NRZ5S_IDX : NRZ5S_ARRAYS, 4, indexed ? 1 + int32_t(r.upto(2)) : 0, count, bv, bi, instances, first, indexed ? 0x7100000000ull + 64 * r.upto(64) : 0, 0x7200000000ull);
        d.fixid = 1 + r.upto(9); d.noff = r.upto(nrz8::MAX_DYN + 1); d.k = r.upto(5); d.first_index = r.upto(7);
        uint64_t ub[6][16] = {};
        v.pipe = 0x1000 + r.upto(9); v.layout = 0x2000 + r.upto(4); v.set = 0x3000 + r.upto(7); v.noff = d.noff; v.off = voff; v.indexed = indexed;
        for (unsigned i = 0; i < d.noff; i++) { d.dyn[i].st = uint8_t(i % 6); d.dyn[i].sl = uint8_t(i); voff[i] = 256 * r.upto(4000); d.dyn[i].addr = 0x7300000000ull + voff[i]; d.dyn[i].rbytes = 256; d.dyn[i].copy = 256;
            d.dyn[i].fixed = r.upto(3) == 0; }   // (a support / c1 block: the template holds the offset itself)
        VkViewport vp{0, 0, float(640u << r.upto(2)), 360, 0, 1}; VkRect2D sc{{0, 0}, {640u << r.upto(2), 360}}; memcpy(v.vp, &vp, sizeof vp); memcpy(v.sc, &sc, sizeof sc);
        v.used = uint16_t(1u << r.upto(4) | (r.upto(2) ? 1u << (4 + r.upto(4)) : 0u)); unsigned nvb = 0;
        for (uint32_t m = v.used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); v.vb[i] = 0x4000 + r.upto(4); v.vbo[i] = 64 * r.upto(1000); d.vb[nvb].at = uint8_t(i); d.vb[nvb].addr = 0x7200000000ull + 4096 * i; d.vb[nvb].size = 4096; ++nvb; }
        d.nvb = nvb; d.nrow = r.upto(4); for (unsigned i = 0; i < d.nrow; i++) { d.row[i].ns = 1; d.row[i].slot = uint8_t(i); d.row[i].samp = 0xff; d.row[i].tex = 0x100 + i; d.row[i].smp = 0; }
        if (indexed) { v.ib = 0x5000; v.ibo = 64 * r.upto(100); v.it = r.upto(2); }
        d.v = &v; uint64_t w[nrz8::MAX_WORDS]; const unsigned nw = nrz8::pack(d, w); CHECK(nw != 0, "values: a template"); if (!nw) continue;
        // the copy at a later draw of the site: the uniform blocks moved; the walk's values are match()'s
        for (unsigned i = 0; i < d.noff; i++) ub[d.dyn[i].st][d.dyn[i].sl] = d.dyn[i].addr + 256 * r.upto(50);
        struct A { uint64_t (*ub_)[16]; const nrz8::Desc *d;
            bool ub(unsigned st, unsigned sl, uint64_t *a, uint64_t *z) const { if (st >= 6 || sl >= 16) return false; *a = ub_[st][sl]; *z = 256; return true; }
            bool vb(unsigned i, uint64_t *a, uint64_t *z) const { for (unsigned j = 0; j < d->nvb; j++) if (d->vb[j].at == i) { *a = d->vb[j].addr; *z = d->vb[j].size; return true; } return false; }
            uint64_t tex(unsigned, unsigned sl) const { return 0x100 + sl; } uint64_t samp(unsigned, unsigned) const { return 0; } };
        nrz5_vals wv; uint32_t woff[nrz8::MAX_DYN]; const int m = nrz8::match(w, &d.site, d.fixid, count, instances, first, bv, bi, A{ub, &d}, &wv, woff); CHECK(m == nrz8::M_OK, "values: the template matches its site (%d)", m); if (m) continue;
        uint8_t side[1024]; const uint32_t sn = nrz5r::pv_put(side, sizeof side, &wv); CHECK(sn != 0, "values: a side record");
        // the values the translation thread resolves now: the walk's, or one field of them changed
        nrz5_vals rv = wv; uint32_t roff[nrz8::MAX_DYN]; memcpy(roff, woff, sizeof roff); rv.off = roff;
        switch (r.upto(14)) {
        case 0: rv.pipe ^= 8; break; case 1: rv.set ^= 16; break; case 2: rv.layout ^= 32; break; case 3: if (rv.noff) roff[r.upto(rv.noff)] += 256; break;
        case 4: rv.vp[r.upto(NRZ5_VP_BYTES)] ^= 1; break; case 5: rv.sc[r.upto(NRZ5_SC_BYTES)] ^= 1; break;
        case 6: { const uint32_t i = uint32_t(__builtin_ctz(rv.used)); if (r.upto(2)) rv.vb[i] ^= 4; else rv.vbo[i] += 64; break; }
        case 7: if (rv.indexed) { if (r.upto(2)) rv.ibo += 64; else rv.it ^= 1; } break; case 8: rv.count += 3; break; case 9: if (rv.indexed) rv.vertex_offset += 1; else rv.first += 1; break;
        case 10: rv.first_instance += 1; break; default: break; }
        const int a = nrz5r::pv_diff(side, sn, &rv);
        const int b = nrz8::expect_diff([&w](unsigned j) { return w[j]; }, &rv, count, instances, first, bv, bi, A{ub, &d});
        CHECK(a == b, "values: the verdict from the slot's words and State is the verdict from the side record (%d / %d)", b, a); if (a) ++differ; else ++same;
    }
    CHECK(same > 20000 && differ > 20000, "values: both verdicts were asked (%ld the same, %ld differ)", same, differ);
    printf("values: %ld draws with the walk's values, %ld with one field changed: expect_diff = pv_diff over the side record\n", same, differ);
}
// ---------------- the set the phone binds (NR-ZERO.md section 36) and the walk's thread with DM_NB_OWN ----------------
// Fort Joy on the Mi 14 Ultra, 20.11: "NVN PERF binds ... offsets_per_bind=7.1", "[nr] desc ... 4.2 ring blocks a draw", and the
// line "[nr] z8, translation thread ... templates published 0, not describable 1428743 (... a static uniform block 1428743 ...)":
// every set has the support blocks of its stages (and c1) as dynamic buffers beside the game's blocks. With no template published
// the walk answered nothing ("slot empty or being written" for every draw record), so every verify of the series compared nothing.
static void phone_test() {
    // vertex stage: support, 3 ring blocks (slots 0 2 3); fragment stage: support, ring block slot 1, c1, 2 textures. dyn_mode: 1 every
    // uniform buffer dynamic (Adreno: 16 allowed) / 2 the game's blocks only / 3 one ring block baked into the set (DM_NR_DESC_SPLIT)
    auto build = [](nr_dplan &p, int dyn_mode, unsigned more_ring) {
        nr_dbind vs[20], fs[8]; uint8_t dv[20], df[8]; unsigned nv = 0, nf = 0; uint32_t b = 0;
        vs[nv] = nr_dbind{b++, NRD_T_UBO, 1, 1392, NRD_S_SUPPORT, 0, 0xffffffffu, 0}; dv[nv++] = dyn_mode == 1;
        const uint32_t vslot[3] = {0, 2, 3}; for (unsigned i = 0; i < 3; i++) { vs[nv] = nr_dbind{b++, NRD_T_UBO, 1, 256, NRD_S_UNIFORM, vslot[i], 0xffffffffu, 0}; dv[nv++] = !(dyn_mode == 3 && i == 2); }
        for (unsigned i = 0; i < more_ring; i++) { vs[nv] = nr_dbind{b++, NRD_T_UBO, 1, 256, NRD_S_UNIFORM, 4 + i, 0xffffffffu, 0}; dv[nv++] = 1; }
        fs[nf] = nr_dbind{b++, NRD_T_UBO, 1, 1392, NRD_S_SUPPORT, 0, 0xffffffffu, 0}; df[nf++] = dyn_mode == 1;
        fs[nf] = nr_dbind{b++, NRD_T_UBO, 1, 256, NRD_S_UNIFORM, 1, 0xffffffffu, 0}; df[nf++] = 1;
        fs[nf] = nr_dbind{b++, NRD_T_UBO, 1, 64, NRD_S_C1, 0, 0xffffffffu, 0}; df[nf++] = dyn_mode == 1;
        fs[nf] = nr_dbind{b++, NRD_T_COMBINED, 1, 0, NRD_S_TEXTURE, 3, 0xffffffffu, 0}; df[nf++] = 0;
        fs[nf] = nr_dbind{b++, NRD_T_COMBINED, 1, 0, NRD_S_TEXTURE, 5, 2, 0}; df[nf++] = 0;
        nr_dstage st[NRD_STAGES]; memset(st, 0, sizeof st);
        st[0].b = vs; st[0].dyn = dv; st[0].n = nv; st[0].ns = nvn_stage[0]; st[0].present = 1; st[4].b = fs; st[4].dyn = df; st[4].n = nf; st[4].ns = nvn_stage[4]; st[4].present = 1;
        return nr_dplan_build(&p, st); };
    struct St { uint64_t ub[6][16], ubz[6][16], vb[16], vbz[16], tex[6][128], samp[6][128]; };
    struct A { const St &S;
        bool ub(unsigned st, unsigned sl, uint64_t *x, uint64_t *z) const { if (st >= 6 || sl >= 16) return false; *x = S.ub[st][sl]; *z = S.ubz[st][sl]; return true; }
        bool vb(unsigned i, uint64_t *x, uint64_t *z) const { if (i >= 16) return false; *x = S.vb[i]; *z = S.vbz[i]; return true; }
        uint64_t tex(unsigned ns, unsigned sl) const { return ns < 6 && sl < 128 ? S.tex[ns][sl] : ~0ull; } uint64_t samp(unsigned ns, unsigned sl) const { return ns < 6 && sl < 128 ? S.samp[ns][sl] : ~0ull; } };
    auto st = std::make_unique<St>(); memset(st.get(), 0, sizeof(St)); const uint64_t ring = 0x7300000000ull;
    for (unsigned ns = 0; ns < 2; ns++) for (unsigned sl = 0; sl < 16; sl++) { st->ub[ns][sl] = ring + 4096 * (1 + ns * 16 + sl); st->ubz[ns][sl] = 256; }
    st->vb[0] = 0x7200000000ull; st->vbz[0] = 8192; st->tex[1][3] = 0x111; st->tex[1][5] = 0x222; st->samp[1][2] = 0x333;
    // the values the tail bound for the draw: 7 offsets in the set's order
    auto vals = [&](const nr_dplan &p, nrz5_vals &v, uint32_t *off) { memset(&v, 0, sizeof v); v.pipe = 0x1001; v.layout = 0x2001; v.set = 0x3001; v.noff = p.ndyn; v.off = off; v.used = 1; v.vb[0] = 0x4000; v.vbo[0] = 64; v.indexed = 1; v.ib = 0x5000; v.it = 1; v.count = 36; v.instances = 1;
        for (unsigned i = 0; i < p.nrow; i++) { const nr_drow &r = p.row[i]; if (r.kind == NRD_K_RING_DYN) off[r.dyn_at] = uint32_t(st->ub[r.ns][r.slot] - ring); else if (r.kind == NRD_K_FIXED_DYN) off[r.dyn_at] = 0x10000u + 1536u * r.binding; } };
    nr_dplan p; uint32_t copy[NRD_MAX_UBO]; for (auto &c : copy) c = 256;
    CHECK(build(p, 1, 0) == NRD_OK && p.ndyn == 7 && p.nubo == 4 && p.nstat == 0 && p.nimg == 2, "phone: the set as the device lays it out - 7 dynamic offsets, 4 of them ring blocks (%u / %u)", unsigned(p.ndyn), unsigned(p.nubo));
    nrz5_vals v; uint32_t off[32]; vals(p, v, off);
    CHECK(nrz8::plan_form(p, v) == nrz8::P_OK, "phone: a set with support / c1 blocks as dynamic buffers can be a template (plan_form says %d; the phone: 2, \"a static uniform block\", for every draw)", nrz8::plan_form(p, v));
    nrz8::Desc d; nrz5_site_make(&d.site, 77, NRZ5S_IDX, 4, 1, 36, 0, 0, 1, 0, 0x7100000000ull, st->vb[0]); d.fixid = 5; d.first_index = 0; d.k = 0;
    const int why = nrz8::describe(p, copy, v, A{*st}, d); CHECK(why == nrz8::P_OK, "phone: the draw is described (%d)", why);
    if (why == nrz8::P_OK) {
        unsigned nfix = 0; for (unsigned i = 0; i < d.noff; i++) nfix += d.dyn[i].fixed; CHECK(d.noff == 7 && nfix == 3 && d.nrow == 2 && d.nvb == 1, "phone: 7 offsets, 3 of them fixed (%u / %u)", d.noff, nfix);
        uint64_t w[nrz8::MAX_WORDS]; const unsigned nw = nrz8::pack(d, w); CHECK(nw == nrz8::HEAD_WORDS + 7 * 3 + 5 + 2 * 3, "phone: the template's words (%u)", nw);
        // the next frame: the game's blocks moved in the ring, the support / c1 blocks did not
        for (unsigned ns = 0; ns < 2; ns++) for (unsigned sl = 0; sl < 16; sl++) st->ub[ns][sl] += 256 * (3 + ns + sl);
        nrz5_vals now; uint32_t noff[32]; vals(p, now, noff);
        nrz5_vals m; uint32_t moff[nrz8::MAX_DYN]; const int mm = nrz8::match(w, &d.site, 5, 36, 1, 0, 0, 0, A{*st}, &m, moff);
        CHECK(mm == nrz8::M_OK && m.noff == 7, "phone: the walk's copy matches the template (%d)", mm);
        if (mm == nrz8::M_OK) { for (unsigned i = 0; i < 7; i++) CHECK(moff[i] == noff[i], "phone: offset %u of the walk's values is the offset the tail binds (%#x / %#x, %s)", i, moff[i], noff[i], d.dyn[i].fixed ? "a support / c1 block" : "a ring block");
            CHECK(m.pipe == now.pipe && m.set == now.set && m.layout == now.layout && m.used == now.used, "phone: the walk's values");
            struct U { const St &S; bool ub(unsigned s2, unsigned sl, uint64_t *x, uint64_t *z) const { if (s2 >= 6 || sl >= 16) return false; *x = S.ub[s2][sl]; *z = S.ubz[s2][sl]; return true; } };
            auto W = [&w](unsigned j) { return w[j]; };
            CHECK(nrz8::expect_diff(W, &now, 36, 1, 0, 0, 0, U{*st}) == 0, "phone: the verdict from the slot's words: the same values");
            for (unsigned i = 0; i < 7; i++) { nrz5_vals x = now; uint32_t xo[32]; memcpy(xo, noff, sizeof xo); x.off = xo; xo[i] += 256;
                CHECK(nrz8::expect_diff(W, &x, 36, 1, 0, 0, 0, U{*st}) == 3, "phone: the verdict sees offset %u (%s) moved", i, d.dyn[i].fixed ? "a support / c1 block" : "a ring block"); } }
        st->tex[1][5] ^= 0x1000; CHECK(nrz8::match(w, &d.site, 5, 36, 1, 0, 0, 0, A{*st}, &m, moff) == nrz8::M_TEX, "phone: another texture in a row of the plan is another answer"); st->tex[1][5] ^= 0x1000;
    }
    // the other layouts
    CHECK(build(p, 2, 0) == NRD_OK && p.ndyn == 4 && p.nubo == 4, "partial dynamic: only the game's blocks are dynamic"); vals(p, v, off);
    { nrz8::Desc e; CHECK(nrz8::describe(p, copy, v, A{*st}, e) == nrz8::P_OK && e.noff == 4, "partial dynamic: described, 4 offsets"); }
    CHECK(build(p, 3, 0) == NRD_OK && p.nstat == 1, "split: a ring block baked into the set"); vals(p, v, off);
    { nrz8::Desc e; CHECK(nrz8::describe(p, copy, v, A{*st}, e) == nrz8::P_STATIC, "split: a set that depends on a block's offset is never a template"); }
    CHECK(build(p, 1, 9) == NRD_OK && p.ndyn == 16, "16 offsets: the device limit"); vals(p, v, off);
    { nrz8::Desc e; nrz5_site_make(&e.site, 78, NRZ5S_IDX, 4, 1, 36, 0, 0, 1, 0, 0x7100000000ull, st->vb[0]); e.fixid = 6; uint64_t w[nrz8::MAX_WORDS];
      CHECK(nrz8::describe(p, copy, v, A{*st}, e) == nrz8::P_OK && nrz8::pack(e, w) == nrz8::HEAD_WORDS + 16 * 3 + 5 + 2 * 3, "16 offsets: a template");
      nrz5_vals m; uint32_t moff[nrz8::MAX_DYN]; CHECK(nrz8::match(w, &e.site, 6, 36, 1, 0, 0, 0, A{*st}, &m, moff) == nrz8::M_OK && m.noff == 16 && !memcmp(moff, off, 16 * 4), "16 offsets: matched, every offset the tail's"); }
    CHECK(build(p, 1, 10) == NRD_OK && p.ndyn == 17, "17 offsets"); vals(p, v, off);
    { nrz8::Desc e; CHECK(nrz8::describe(p, copy, v, A{*st}, e) == nrz8::P_MANY, "17 offsets: more than a template holds"); }
    { nr_dplan q; build(q, 1, 0); vals(q, v, off); v.noff = 6; nrz8::Desc e; CHECK(nrz8::describe(q, copy, v, A{*st}, e) == nrz8::P_STATIC, "a tail that bound another number of offsets than the plan has is not described"); }
    // the walk's own thread: DM_NB_OWN is the game's default (the app's JNI block) - the thread must start with it
    CHECK(nrz8::walker_thread(1, 1) == 1, "thread: DM_NR_Z8_THREAD=1 with DM_NB_OWN=1 gets the walker thread (the phone: \"the submitting thread walks\" - DM_NR_Z8_VIEW / _VAL went off with it)");
    CHECK(nrz8::walker_thread(1, 2) == 1 && nrz8::walker_thread(1, 0) == 1 && nrz8::walker_thread(0, 1) == 0 && nrz8::walker_thread(0, 0) == 0, "thread: the switch decides");
    printf("phone: the set of Fort Joy (7 dynamic offsets: 4 ring blocks + 2 support + c1) is a template, its offsets of both kinds are the tail's; a ring block baked into the set is refused; 16 offsets fit, 17 do not; the walker thread starts with DM_NB_OWN\n");
}

// ---------------- the ranges of a submission: a number a range ----------------
static void ranges_test() {
    auto R = std::make_unique<nrz8::Ranges>(); R->begin();
    CHECK(R->id_of(0x1000, 64, 1) == 1 && R->id_of(0x2000, 64, 1) == 2 && R->id_of(0x1000, 64, 1) == 1 && R->id_of(0x1000, 128, 1) == 3 && R->id_of(0x1000, 64, 2) == 4 && R->id_of(0x2000, 64, 1) == 2, "ranges: numbers in the order of first use, a repeat keeps its number");
    R->begin(); CHECK(R->id_of(0x2000, 64, 1) == 1 && R->id_of(0x1000, 64, 2) == 2 && R->id_of(0x1000, 64, 1) == 3, "ranges: the next submission numbers from 1");
    Rng r{17}; std::map<std::pair<uint64_t, uint64_t>, uint32_t> seen; R->begin(); uint32_t n = 0; bool full = false;
    for (int i = 0; i < 40000; i++) { const uint64_t a = 0x7000000 + 64 * r.upto(6000), z = 64 << r.upto(2); const uint32_t id = R->id_of(a, z, 1); auto it = seen.find({a, z});
        if (!id) { full = true; CHECK(it == seen.end(), "ranges: a numbered range keeps its number when the table is full"); continue; }
        if (it == seen.end()) { CHECK(id == ++n, "ranges: a new range takes the next number"); seen[{a, z}] = id; } else CHECK(it->second == id, "ranges: the same range, the same number"); }
    CHECK(full && n <= nrz8::Ranges::MAX_ID, "ranges: the table fills up (%u numbers) and then answers 0", n);
    for (int g = 0; g < 3; g++) { R->begin(); CHECK(R->id_of(0x7000000, 64, 1) == 1, "ranges: a full table is empty again in the next submission"); }
    uint8_t used_vb = 0; auto vb = [&](uint32_t i, uint64_t *a, uint64_t *z) { used_vb |= uint8_t(1u << i); *a = 0x9000 + 0x100 * i; *z = 256; return true; };
    R->begin(); const uint64_t w = nrz8::ranges_word(*R, 0x5, vb, true, 0xA000, 600);
    CHECK(w == (1ull | 2ull << 16 | 3ull << 48) && used_vb == 0x5, "ranges: the word of a draw: bindings 0 and 2, then the index range (%#llx)", (unsigned long long)w);
    CHECK(nrz8::ranges_word(*R, 0xf, vb, false, 0, 0) == 0, "ranges: more than three bindings have no word");
    CHECK(nrz8::ranges_word(*R, 0x4, vb, false, 0, 0) == 2, "ranges: a repeat inside another draw");
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

// ---------------- PHONE CENSUS (NR-ZERO.md section 39): the refusals of the phone series R8 in one world ----------------
// Fort Joy, standing (R8_v1, Adreno 750, DM_NR_Z8=2, 40 s): 1 493 draw records a submission, the walk answered 41.5 %; not: slot empty
// 22.5 %, vertex ranges differ 18.1 %, form 13.8 %, another site 3.8 %, texture handles 0.2 %. What the phone's other lines of the same
// windows say these are:
//   - "[nr] shcache ... skipped 403 a frame" and "[nr] draw: 1276542 draw records, 930411 asked nr_draw": the shadow cache drops 27 %
//     of the records in the prologue of draw() - they never reach the lane and are never published (slot empty);
//   - "[nr] mat ... swapped" +112 306 and "[nr] surf ... swapped" +64 676 in the window (Mali SIM: +397 710 / +61 408): NR-MAT and
//     NR-SH-SURF swap the fragment shader of a depth-only / surface draw for a twin; xbg_game.cpp z8g took a twin for a changed State
//     - never published (slot empty, or form when the slot held a template), never answered;
//   - a second vertex stream that moves between frames under the same site words (vertex stream 0 is a site word);
//   - about 1 500 distinct sites in a direct-mapped table of 8 192 slots: two sites of one slot overwrite each other (another site).
// The world: 8 passes a frame, 180 draws each, every draw with its OP_X_NR_STATE; pass 0 a shadow cascade the shadow cache drops three
// frames in four; passes 1-2 depth-only (fragment programs 106 / 107 have twins 112 / 113 there; a word of a uniform block says no
// now and then - the NR-MAT gate); passes 3-7 colour, one draw of pass 7 LIGHTFUSE's (State changed: never answered); a quarter of the
// meshes have a vertex stream 1 that moves every frame; two draws a frame get another mesh (first sight).
struct PWorld {
    Rng r; Mem mem; uint32_t fn = 0, head = 0; const uint64_t ring = 0x7300000000ull;
    struct M { uint32_t key; int vp, fp; uint64_t dsb; uint32_t poly, color; uint64_t blend0; uint32_t attr[3], stride0, stride1; uint64_t tex[4], smp[4]; };
    struct E { uint64_t vb0, vb0s, ibuf; int32_t count, base_vertex, itype; bool moves; };
    std::vector<M> mats; std::vector<E> meshes; std::vector<std::pair<uint32_t, uint32_t>> order;   // (material, mesh) of each place of a frame
    enum { PASSES = 8, PER = 180, MESHES = 720 };
    explicit PWorld(uint64_t seed) : r{seed * 0x9E3779B97F4A7C15ull + 99} {
        mem.r = &r; mats.resize(64); meshes.resize(MESHES);
        for (uint32_t i = 0; i < mats.size(); i++) { M &m = mats[i];
            m.key = 1 + (i * 37) % 160; m.vp = int(r.upto(6)); m.fp = i < 16 ? (i % 4 == 3 ? 8 + int(i % 2) : 6 + int(i % 2)) : 8 + int(r.upto(4));   // 0..15 depth-only (12 of them twin-able), 16..63 colour
            m.dsb = r.upto(4) * 0x0101010101ull; m.poly = r.upto(3); m.color = r.upto(3) << 8; m.blend0 = uint64_t(r.upto(3)) << 3;
            for (auto &a : m.attr) a = r.upto(4) << 8;
            m.stride0 = 16 + 4 * r.upto(3); m.stride1 = 8 + 4 * r.upto(2);
            for (int k = 0; k < 4; k++) { m.tex[k] = 0x100 + r.upto(400); m.smp[k] = 0x200 + r.upto(6); } }
        mats[63].key = 170; mats[63].fp = 11;   // LIGHTFUSE's Light draw
        for (uint32_t i = 0; i < MESHES; i++) { E &e = meshes[i]; e.vb0 = 0x7200000000ull + 0x10000ull * i; e.vb0s = 4096 * (1 + r.upto(4)); e.ibuf = 0x7100000000ull + 0x10000ull * i;
            e.count = int32_t(3 * (1 + r.upto(800))); e.base_vertex = int32_t(r.upto(50)); e.itype = 1 + int32_t(r.upto(2)); e.moves = r.upto(4) == 0; }
        for (uint32_t p = 0; p < PASSES; p++) for (uint32_t j = 0; j < PER; j++)
            order.push_back({p < 3 ? (j / 4 * 7 + p) % 16 : p == 7 && j == 0 ? 63u : 16 + (j / 4 * 5 + p * 3) % 47, (p * PER + j * 13) % MESHES});   // (a material's draws in runs of four, as an engine sorts them)
    }
    void pass(uint32_t p) {
        p_set_rt rt; memset(&rt, 0, sizeof rt); rt.n = p < 3 ? 0 : 1; rt.color[0] = p < 3 ? nullptr : reinterpret_cast<const tex_t *>(uintptr_t(0x9000 + 64 * (p & 3))); rt.depth = reinterpret_cast<const tex_t *>(uintptr_t(0x9800 + 64 * p));
        mem.put(OP_SET_RT, rt); uint32_t z = p; mem.put(OP_T_PASS, z);
        p_viewports v{}; v.first = 0; v.n = 1; v.v[0][2] = p < 3 ? 2048.f : 640.f; v.v[0][3] = p < 3 ? 2048.f : 360.f; mem.put(OP_VIEWPORTS, v);
        p_scissor sc{0, 0, p < 3 ? 2048 : 640, p < 3 ? 2048 : 360}; mem.put(OP_SCISSOR, sc);
        if (p == 0) { p_set_pool sp{&g_pool[0]}; mem.put(OP_SET_TEX_POOL, sp); sp.pool = &g_pool[2]; mem.put(OP_SET_SAMP_POOL, sp); }
        last = nullptr; state_block(true);
    }
    // a state block of the pass (DEF at the pass, a REF before every draw: the phone walks 4.8 records a draw record) - vertex buffer 2,
    // which no pipeline of the world reads (no fix group)
    void state_block(bool def) {
        const uint32_t sz = 8 + 8 + 16, content = def ? sz + 8 : 0;
        uint8_t *sb = mem.rec(OP_X_NB_STATE, uint32_t(sizeof(nbs_hdr) + sizeof(nbs_blk) + content)); nbs_hdr h{NBS_VER, 1, 0, fn}; memcpy(sb, &h, sizeof h);
        nbs_blk k{NBS_K_VB, uint8_t(def ? NBS_B_DEF : NBS_B_REF), 3, content}; memcpy(sb + 8, &k, sizeof k);
        if (def) { uint8_t *c = sb + 16; Mem::hdr(c, OP_BIND_VBUF, sz); p_bind_vbuf vb{}; vb.first = 2; vb.n = 1; vb.r[0] = NVNbufferRange{0x7700000000ull, 4096}; memcpy(c + 8, &vb, 8 + 16); Mem::hdr(c + sz, OP_END, 8); }
    }
    const M *last = nullptr;
    void material(const M &m, const E &e, uint32_t place) {   // (the material's state when it changes, the vertex buffers always - as the phone's 85-byte packets)
        nr3_rec z; memset(&z, 0, sizeof z);
        if (last == &m) { z.vb = 3; z.vba[0] = e.vb0; z.vbs[0] = e.vb0s; z.vba[1] = e.moves ? 0x7600000000ull + uint64_t(fn % 512) * 0x100000ull + uint64_t(place) * 256 : e.vb0 + 0x8000; z.vbs[1] = 4096; nr3_put(&z, mem.rec(OP_X_NR_STATE, nr3_size(&z))); return; }
        last = &m; z.nprog = 2; z.prog[0] = uint64_t(uintptr_t(&g_prog[m.vp])); z.pstage[0] = 1; z.prog[1] = uint64_t(uintptr_t(&g_prog[m.fp])); z.pstage[1] = 2;
        z.have = NR3_H_DS | NR3_H_POLY | NR3_H_COLOR | NR3_H_VATTR | NR3_H_VSTREAM; z.ds = m.dsb; z.poly = m.poly; z.color = m.color; z.blend = 1; z.blendb[0] = m.blend0;
        z.nvattr = 3; memcpy(z.vattr, m.attr, sizeof m.attr); z.nstream = 2; z.vstream[0] = m.stride0; z.vstream[1] = m.stride1;
        z.vb = 3; z.vba[0] = e.vb0; z.vbs[0] = e.vb0s; z.vba[1] = e.moves ? 0x7600000000ull + uint64_t(fn % 512) * 0x100000ull + uint64_t(place) * 256 : e.vb0 + 0x8000; z.vbs[1] = 4096;
        nr3_put(&z, mem.rec(OP_X_NR_STATE, nr3_size(&z)));
    }
    void draw(const M &m, const E &e) {
        nbd_shadow s; memset(&s, 0, sizeof s);
        for (uint32_t i = 0; i < uint32_t(m.fp) % 4; i++) { const int slot = int((i * 5 + uint32_t(m.fp)) % 16); nbd_add(&s, NBD_B_TEX, 1, slot, m.tex[i], 0); nbd_add(&s, NBD_B_SAMP, 1, slot, m.smp[i], 0); }
        for (uint32_t i = 0; i < 1 + uint32_t(m.vp) % 3; i++) { head = (head + 256 * (1 + r.upto(3))) & 0xffffff; nbd_add(&s, NBD_B_UBO, int(i & 1), int(i), ring + head, 256); }
        s.h.engine_op = 36; s.h.flags = NBD_F_NR; s.h.prim = 4; s.h.count = e.count; s.h.draw = NBD_DRAW_IDX; s.h.itype = e.itype; s.h.base_vertex = e.base_vertex; s.h.instances = 1; s.h.ibuf = e.ibuf;
        const uint32_t size = nbd_size(&s) + uint32_t(sizeof(nrd_tail)); uint8_t *rc = mem.rec(OP_X_NB_DRAW, size); nbd_pack(&s, rc, size); nrd_tail_put(rc, size, m.key, 1, 0, 3);
    }
    Frame frame() {
        ++fn; Frame f; f.g = Gens{1, 1, ring - 4096}; f.g.cached = fn > 1 && fn % 4 != 1; f.g.warm = fn > 4;
        for (int c = 0; c < 2; c++) order[r.upto(uint32_t(order.size()))].second = r.upto(MESHES);
        for (uint32_t p = 0; p < PASSES; p++) {
            if (p == 0 || p == 4) { if (p) mem.finish(); f.handles.push_back(uint64_t(uintptr_t(mem.begin()))); }
            pass(p);
            for (uint32_t j = 0; j < PER; j++) { const auto &o = order[p * PER + j]; state_block(false); material(mats[o.first], meshes[o.second], j); draw(mats[o.first], meshes[o.second]); }
        }
        mem.finish(); return f;
    }
};
static void census_test(int frames) {
    g_phone = true;
    PWorld w(5); std::vector<Frame> prog; for (int i = 0; i < frames; i++) prog.push_back(w.frame());
    setup(Cfg{0, 256, 64, 64, 0}); const auto ref = run(prog, 0, 1); finish(); const uint64_t ref_draws = tot.recorded, dropped = tot.dropped;
    for (int wk : {0, 2}) {
        setup(Cfg{1, 256, 64, 64, 0}); { const auto logs = run(prog, 1, 9, nullptr, wk != 0, wk);   /* (the submitting thread walks each frame right before it: the shares do not depend on timing) */ for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "census apply (%s): frame %zu's call log is today's", wk ? "own thread" : "submitting thread", f); }
        const Tot t = tot; finish();
        CHECK(t.valid + t.recorded == ref_draws, "census apply: every draw taken or recorded once (%llu + %llu of %llu)", (unsigned long long)t.valid, (unsigned long long)t.recorded, (unsigned long long)ref_draws);
        const double sh = t.lane ? double(t.lane_ans) / double(t.lane) : 0.;
        printf("census (%s, %d frames, %llu draw records, %llu dropped by the prologue): drawn after the warm-up %llu, answered %llu (%.1f%%; no prologue %llu of %llu, twin %llu of %llu, State changed %llu of %llu); "
               "the walk: values for %llu of %llu records, not: site %llu fix %llu tex %llu vb (answered, no values) %llu ubo %llu form %llu empty %llu; packets taken %llu\n", wk ? "the walk on its own thread" : "the walk on the submitting thread", frames,
               (unsigned long long)t.draws, (unsigned long long)dropped, (unsigned long long)t.lane, (unsigned long long)t.lane_ans, 100. * sh, (unsigned long long)t.lane_pro_ans[0], (unsigned long long)t.lane_pro[0],
               (unsigned long long)t.lane_pro_ans[1], (unsigned long long)t.lane_pro[1], (unsigned long long)t.lane_pro_ans[2], (unsigned long long)t.lane_pro[2], (unsigned long long)t.w_values, (unsigned long long)t.w_draws,
               (unsigned long long)t.w_miss[nrz8::M_SITE], (unsigned long long)t.w_miss[nrz8::M_FIX], (unsigned long long)t.w_miss[nrz8::M_TEX], (unsigned long long)t.w_miss[nrz8::M_NOVAL], (unsigned long long)t.w_miss[nrz8::M_UBO],
               (unsigned long long)t.w_miss[nrz8::M_FORM], (unsigned long long)t.w_miss[nrz8::M_EMPTY], (unsigned long long)t.valid);
        CHECK(t.lane > 0 && sh >= 0.95, "census: the walk answers at least 95%% of the drawn draws whose words arrived (%.1f%% of %llu) - the phone R8: 41.5%% of the records", 100. * sh, (unsigned long long)t.lane);
        CHECK(t.lane_pro[1] > 0 && t.lane_pro_ans[1] * 10 >= t.lane_pro[1] * 9, "census: a twin draw is answered (%llu of %llu) - only its fragment program is not the walk's copy, and the answer names the twin", (unsigned long long)t.lane_pro_ans[1], (unsigned long long)t.lane_pro[1]);
        CHECK(t.lane_pro[2] > 0 && t.lane_pro_ans[2] == 0, "census: a draw whose State the prologue changed is never answered (%llu of %llu)", (unsigned long long)t.lane_pro_ans[2], (unsigned long long)t.lane_pro[2]);
        setup(Cfg{2, 256, 64, 64, 0}); { const auto logs = run(prog, 2, 11, nullptr, wk != 0, wk); for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "census verify: frame %zu's call log is today's", f); }
        const Tot v = tot; finish();
        CHECK(v.a_bad == 0 && v.a_cmp > v.lane / 2 && v.r.v_bad == 0 && v.r.v_cmp == v.r.v_same, "census verify: answers compared %llu bad %llu, packets compared %llu bad %llu", (unsigned long long)v.a_cmp, (unsigned long long)v.a_bad,
              (unsigned long long)v.r.v_cmp, (unsigned long long)v.r.v_bad);
    }
    g_phone = false;
}

// BENCH: the walk of the census world on one thread (no translation thread in between: the walk alone is clocked), warm frames only.
static void bench() {
    g_phone = true; PWorld w(5); std::vector<Frame> prog; for (int i = 0; i < 40; i++) prog.push_back(w.frame());
    struct B { unsigned bits; const char *what; } bs[] = {{1, "the walk as the engine's (no test checks)"}, {1 | 4, "no packets"}, {1 | 8, "no range numbers"}, {1 | 2, "no template (look, values, packets, ranges)"},
                                                       {1 | 2 | 16, "no template, no fix id"}, {1 | 32, "a draw record: its binds only"}, {1 | 64, "no values records (DM_NR_Z8_VAL=1)"}, {0, "with the test's own checks"}};
    double base = 0;
    for (const B &b : bs) { double best = 1e18, per_rec = 0; uint64_t d = 0, r = 0;
        for (int rep = 0; rep < 3; rep++) { g_bench = b.bits; setup(Cfg{1, 256, 64, 64, 0}); run(prog, 1, 9, nullptr, false, 0); finish();
            const double x = tot.warm_draws ? double(tot.warm_ns) / double(tot.warm_draws) : 0.; if (x < best) { best = x; per_rec = double(tot.warm_ns) / double(tot.warm_records); d = tot.warm_draws; r = tot.warm_records; } }
        if (b.bits == 1) base = best;
        printf("bench %-48s %7.0f ns a draw record (%5.0f ns a record; %.0f draw records and %.0f records a frame)%s", b.what, best, per_rec, double(d) / 36., double(r) / 36., b.bits == 1 ? "\n" : "");
        if (b.bits != 1) printf("  -> %.0f ns\n", base - best); }
    g_bench = 0; g_phone = false;
}

int main(int argc, char **argv) {
    const int frames = argc > 1 ? atoi(argv[1]) : 60;
    for (int i = 0; i < 16; i++) { memset(&g_prog[i], 0, sizeof g_prog[i]); g_prog[i].nstage = 2; g_prog[i].serial = 100 + uint64_t(i); }
    for (int i = 0; i < 4; i++) { memset(&g_mem[i], 0, sizeof g_mem[i]); g_pool[i].pool = &g_mem[i]; g_pool[i].off = 64 * i; g_pool[i].n = 100 + i; }
    if (argc > 1 && !strcmp(argv[1], "bench")) { bench(); return 0; }
    dict_test(); table_race(); marks_test(); ranges_test(); values_test(); phone_test(); census_test(frames > 24 ? 24 : frames);
    uint64_t described = 0, not_desc = 0, taken = 0, answered = 0, draws = 0, vcmp = 0, th_answered = 0, th_taken = 0, th_got = 0, th_asked = 0, th_waits = 0, th_vcmp = 0, th_dig = 0, th_splits = 0, th_vask = 0, th_vrep = 0, th_known = 0, th_kcmp = 0;
    for (uint64_t seed = 1; seed <= 3; seed++) {
        World w(seed); std::vector<Frame> prog; for (int i = 0; i < frames; i++) prog.push_back(w.frame());
        setup(Cfg{0, 256, 64, 64, 0}); const auto ref = run(prog, 0, seed); finish(); const uint64_t ref_draws = tot.recorded;
        const Cfg shapes[] = {{1, 256, 64, 64, 0}, {2, 256, 64, 64, 0}, {3, 256, 64, 64, 0}, {2, 16, 16, 2, 0}, {3, 16, 16, 2, 7}, {3, 65536, 1024, 1024, 0}};
        for (const Cfg &cf : shapes) {
            const bool first = &cf == &shapes[0] && frames >= 40;   // (the shares are asked of a run long enough to warm the table up)
            setup(cf); { const auto logs = run(prog, 1, seed * 77 + cf.mode);
                for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "apply: seed %llu queue mode %d flush %u: frame %zu's call log is today's (%zu / %zu bytes)", (unsigned long long)seed, cf.mode, cf.flush, f, logs[f].size(), ref[f].size()); }
            const Tot t = tot; finish(); described += t.described; not_desc += t.not_desc;
            CHECK(t.valid + t.recorded == ref_draws, "apply: every draw taken or recorded once (%llu + %llu of %llu)", (unsigned long long)t.valid, (unsigned long long)t.recorded, (unsigned long long)ref_draws);
            CHECK(first ? t.answered > t.draws / 4 && t.valid > t.draws / 8 : t.answered > 0 && t.valid > 0, "apply: the walk's answers are used (%llu answered, %llu taken of %llu)", (unsigned long long)t.answered, (unsigned long long)t.valid, (unsigned long long)t.draws);
            taken += t.valid; answered += t.answered; draws += t.draws;
            setup(cf); { const auto logs = run(prog, 2, seed * 177 + cf.mode);
                for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "verify: seed %llu queue mode %d: frame %zu's call log is today's", (unsigned long long)seed, cf.mode, f); }
            const Tot v = tot; finish();
            CHECK(v.a_bad == 0 && (first ? v.a_cmp > v.draws / 4 : v.a_cmp > 0), "verify: answers compared %llu, bad %llu", (unsigned long long)v.a_cmp, (unsigned long long)v.a_bad);
            CHECK(v.r.v_bad == 0 && v.r.v_cmp > 0 && v.r.v_cmp == v.r.v_same, "verify: packets compared %llu, same %llu, bad %llu", (unsigned long long)v.r.v_cmp, (unsigned long long)v.r.v_same, (unsigned long long)v.r.v_bad);
            vcmp += v.r.v_cmp;
            // ---- the walk on its own thread ----
            setup(cf); { const auto logs = run(prog, 1, seed * 277 + cf.mode, nullptr, true, 1);
                for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "thread apply: seed %llu queue mode %d flush %u: frame %zu's call log is today's (%zu / %zu bytes)", (unsigned long long)seed, cf.mode, cf.flush, f, logs[f].size(), ref[f].size()); }
            const Tot ta = tot; finish();
            CHECK(ta.valid + ta.recorded == ref_draws, "thread apply: every draw taken or recorded once (%llu + %llu of %llu)", (unsigned long long)ta.valid, (unsigned long long)ta.recorded, (unsigned long long)ref_draws);
            CHECK(ta.m_got > 0 && ta.m_got == ta.dig_cmp, "thread apply: words arrived (%llu) and each came with the digest", (unsigned long long)ta.m_got);
            th_vask += ta.v_ask; th_vrep += ta.v_rep; th_known += ta.r.known; th_kcmp += ta.r.k_cmp;
            CHECK(ta.r.k_bad == 0, "values: what the model named for a tail draw is what the chunks hold (apply: %llu compared, %llu bad)", (unsigned long long)ta.r.k_cmp, (unsigned long long)ta.r.k_bad);
            th_answered += ta.answered; th_taken += ta.valid; th_got += ta.m_got; th_asked += ta.draws; th_waits += ta.waits; th_dig += ta.dig_cmp; th_splits += ta.splits;
            setup(cf); { const auto logs = run(prog, 2, seed * 377 + cf.mode, nullptr, true, 1);
                for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "thread verify: seed %llu queue mode %d: frame %zu's call log is today's", (unsigned long long)seed, cf.mode, f); }
            const Tot tv = tot; finish();
            CHECK(tv.a_bad == 0 && tv.r.v_bad == 0 && tv.r.v_cmp == tv.r.v_same, "thread verify: answers compared %llu bad %llu, packets compared %llu bad %llu", (unsigned long long)tv.a_cmp, (unsigned long long)tv.a_bad, (unsigned long long)tv.r.v_cmp, (unsigned long long)tv.r.v_bad);
            th_vcmp += tv.r.v_cmp; th_dig += tv.dig_cmp; th_kcmp += tv.r.k_cmp;
            CHECK(tv.r.k_bad == 0, "values: verify - the old verdict finds in the chunks what the model named (%llu compared, %llu bad)", (unsigned long long)tv.r.k_cmp, (unsigned long long)tv.r.k_bad);
            if (first) {
                setup(cf); { const auto logs = run(prog, 1, seed * 477, nullptr, true, 2); for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "thread apply, walker ahead: frame %zu's call log is today's", f); }
                const Tot tw = tot; finish();
                CHECK(tw.m_got == tw.draws && tw.m_not == 0 && tw.m_nobase == 0, "thread, walker ahead: every record has its words (%llu of %llu)", (unsigned long long)tw.m_got, (unsigned long long)tw.draws);
                CHECK(tw.answered > tw.draws / 4 && tw.valid > tw.draws / 8 && tw.valid + tw.recorded == ref_draws, "thread, walker ahead: the walk's answers are used as on the submitting thread (%llu answered, %llu taken of %llu)",
                      (unsigned long long)tw.answered, (unsigned long long)tw.valid, (unsigned long long)tw.draws);
                CHECK(tw.splits > 0, "thread: runs were split (%llu)", (unsigned long long)tw.splits);
                CHECK(tw.v_ask > tw.draws / 4 && tw.v_rep > tw.v_ask / 10, "ranges: numbers arrived (%llu) and ranges repeat inside a frame (%llu)", (unsigned long long)tw.v_ask, (unsigned long long)tw.v_rep);
                th_vask += tw.v_ask; th_vrep += tw.v_rep;
                setup(cf); { const auto logs = run(prog, 1, seed * 577, nullptr, true, 3); for (size_t f = 0; f < prog.size(); f++) CHECK(logs[f] == ref[f], "thread apply, translation thread first: frame %zu's call log is today's", f); }
                const Tot tf = tot; finish();
                CHECK(tf.m_got == 0 && tf.answered == 0 && tf.valid == 0 && tf.recorded == ref_draws && tf.waits == prog.size(), "thread, translation thread first: nothing arrives, nothing is taken, every frame waits for its walk (%llu words, %llu waits)",
                      (unsigned long long)tf.m_got, (unsigned long long)tf.waits);
                printf("seed %llu host clock: the walk %.0f ns a draw record on the submitting thread, %.0f ns on its own thread (all records of the frame: state, binds, fix id, template, packets)\n", (unsigned long long)seed,
                       t.w_draws ? double(t.walk_ns) / double(t.w_draws) : 0., tw.w_draws ? double(tw.walk_ns) / double(tw.w_draws) : 0.);
                printf("seed %llu thread: mixed order: words for %llu of %llu records (not yet written %llu, walk not begun %llu), answered %llu, packets taken %llu, runs split %llu, waits at a frame's end %llu; walker ahead: answered %llu (%.0f%%), taken %llu (%.0f%%)\n",
                       (unsigned long long)seed, (unsigned long long)ta.m_got, (unsigned long long)ta.draws, (unsigned long long)ta.m_not, (unsigned long long)ta.m_nobase, (unsigned long long)ta.answered, (unsigned long long)ta.valid, (unsigned long long)ta.splits,
                       (unsigned long long)ta.waits, (unsigned long long)tw.answered, 100. * double(tw.answered) / double(tw.draws), (unsigned long long)tw.valid, 100. * double(tw.valid) / double(tw.draws));
            }
            if (&cf == &shapes[0]) printf("seed %llu: %llu draw records (%llu drawn); apply: answered without State %llu (%.0f%%), packets taken %llu (%.0f%%), runs %llu complete %llu, values differ %llu, cache %llu; "
                "walk: values for %llu of %llu, misses site %llu fix %llu tex %llu vb %llu ubo %llu; verify: answers %llu bad %llu, packets %llu bad %llu, digests %llu\n", (unsigned long long)seed,
                (unsigned long long)t.draws, (unsigned long long)ref_draws, (unsigned long long)t.answered, 100. * double(t.answered) / double(t.draws), (unsigned long long)t.valid, 100. * double(t.valid) / double(t.draws),
                (unsigned long long)t.r.started, (unsigned long long)t.r.complete, (unsigned long long)t.r.cut[nrz5r::CUT_VALUES], (unsigned long long)t.r.cut[nrz5r::CUT_CACHE], (unsigned long long)t.w_values, (unsigned long long)t.w_draws,
                (unsigned long long)t.w_miss[nrz8::M_SITE], (unsigned long long)t.w_miss[nrz8::M_FIX], (unsigned long long)t.w_miss[nrz8::M_TEX], (unsigned long long)t.w_miss[nrz8::M_NOVAL], (unsigned long long)t.w_miss[nrz8::M_UBO],
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
    CHECK(th_got > 0 && th_answered > 0 && th_taken > 0, "thread: words arrived and were used in the mixed order (%llu words, %llu answered, %llu taken)", (unsigned long long)th_got, (unsigned long long)th_answered, (unsigned long long)th_taken);
    printf("thread (the walk on its own thread, all shapes): %llu draw records, words arrived for %llu, answered without State %llu, packets taken %llu, runs split %llu, packets compared by verify %llu, digests compared %llu, waits for the walker %llu\n",
           (unsigned long long)th_asked, (unsigned long long)th_got, (unsigned long long)th_answered, (unsigned long long)th_taken, (unsigned long long)th_splits, (unsigned long long)th_vcmp, (unsigned long long)th_dig, (unsigned long long)th_waits);
    CHECK(th_known > 0 && th_kcmp > 0, "values: tail draws were taken by the verdict without the chunks (%llu) and compared with the old one (%llu)", (unsigned long long)th_known, (unsigned long long)th_kcmp);
    printf("values (the verdict from the translation thread's own memory): %llu tail draws taken without reading the walker's chunks, %llu verdicts compared with the chunks\n", (unsigned long long)th_known, (unsigned long long)th_kcmp);
    printf("ranges (the walker's numbers against the translation thread's State): %llu ranges asked by number, %llu of them a repeat inside their frame (%.0f%%)\n", (unsigned long long)th_vask, (unsigned long long)th_vrep, th_vask ? 100. * double(th_vrep) / double(th_vask) : 0.);
    CHECK(described > 0 && not_desc == 0, "phone: every draw the model recorded on the submitting-thread runs was described (%llu, not %llu)", (unsigned long long)described, (unsigned long long)not_desc);
    printf("phone (sets laid out by nr_desc.c with support / c1 blocks as dynamic buffers): %llu recorded draws described for a template, not describable %llu\n", (unsigned long long)described, (unsigned long long)not_desc);
    printf("nr_z8_test: %ld checks, %ld failed; %llu draw records, %llu answered without State, %llu packets taken, %llu packets compared by verify\n", checks, fails, (unsigned long long)draws, (unsigned long long)answered,
           (unsigned long long)taken, (unsigned long long)vcmp);
    return fails ? 1 : 0;
}
