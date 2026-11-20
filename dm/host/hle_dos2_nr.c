// [DOS2] NR-01: engine object registry of the native renderer (groundwork; changes nothing in rendering). main = main.raw of update
// v786432, build 4979b200. r = the renderer ([main+0x2bbdc68]); engine handles are u64 (low 32 bits index, bits 32..53 generation).
//   DM_NR=0 / unset  nothing: no hook is installed, the NVN tap lines of hle_nvn.c (NRT(...), nvn_nr_tap.inc) are a load and a branch.
//   DM_NR=1          hooks one level above NVN fill dm/gfx/nr/nr_reg.h: every engine object by its handle, with the NVN object the same
//                    engine call created (the tap attributes nvnTextureInitialize / nvnProgramSetShaders / nvnSamplerInitialize /
//                    nvnBufferInitialize to the engine call that is open on the calling thread: a per-thread bracket stack). Census
//                    lines "[nr] ..." every 10 s.
//   DM_NR=2          the same + checks (engine format -> NVN format the builder got, size / mips / target, packaged layout, the engine's
//                    own record against the tap, sampler and state enums); the first 16 mismatches are red lines, all are counted.
//   DM_NR_TEX=1..3   NR-05, material textures by engine events (implies the registry, DM_NR=1 when DM_NR is unset); see the NR-05 block
//                    below and docs/dos2/nr/NR-05.md. 0 / unset: nothing of it (no worker, no table, the engine asks nothing).
//   DM_NR_TEXSTREAM=1..2  NR-22, the NR-05 worker also makes the VkImage + memory and fills its own staging (implies DM_NR_TEX=1 when
//                    DM_NR_TEX is unset; needs DM_NR_TEX 1 or 3); see the NR-22 lines of the NR-05 block and docs/dos2/nr/NR-22.md.
//   DM_NR_DUMP=1     with 1 / 2: the registry as TSV files (nr_reg.h nr_tsv_name: nr_textures.tsv, nr_shaders.tsv, ...) in
//                    <DM_NR_DUMP_DIR | DM_RUNTIME_DIR | dm>, rewritten at the first census line and then every 30 s, at most 64 times.
// Hooks (dn_hook of hle_dos2_native.c: 16-byte jump over 4 prologue instructions, none PC-relative, + trampoline; every site is compared
// word for word first - a site that differs gets one red line and stays unhooked, the rest work):
//   0x91e2a0 CreateTexture(r, handle, void **initData, w3 engineFmt, w4 w, w5 h, w6 depth, w7 mips; STACK: byte, layers, MSAA code,
//            u16 flags, u8 flags2, NVN flags override, NVN format override, pool alloc desc*)
//   0x91f2e0 CreateTextureFromImage(r, handle, image*, w3 bit0 = sRGB)   image: +0 engine format, +4 flags (bit1 cube), +8 NVN format
//            override (-1 none), +0xc packaged layout word (-1 = linear data, WriteTexels), +0x18 w, +0x20 h, +0x28 depth, +0x38 mips, +0x40 layers
//   0x93c3a0 window textures (x0 = window: handles at +0x68 / +0x70, engine format at +0x148)
//   0x91d630 deferred texture destroy (x0 = device, x1 = a COPY of the texture record: +0 the NVN texture object; returns 1 once it
//            really finalized it) - the registry entry dies by NVN object
//   0x914bb0 shared RT storage reservation (r, key, engineFmt, w, h): creates NO texture, only sizes a pool entry (counted by key)
//   0x921e40 CreateSampler(r, filter, wrapU, wrapV, wrapW, maxAniso, compare) -> handle
//   0x8f8430 ShaderManager::Get(mgr, FixedString *name, flag) -> the map's value; 0x92d320 LoadShaderFile(path, flag);
//   0x915ad0 CreateShader(r, vs, fs, cs) -> handle. Get calls Load calls Create on one thread, so the brackets nest and the name / path
//            of the outer calls name the shader. FixedString = pointer to the characters (u16 length at chars - 8); the path is the
//            engine's 16-byte string (byte 0 bit 0 clear: length = byte0 >> 1, characters from +1; set: u32 length at +4, char* at +8).
//   0x917100 CreateVertexFormat(r, elements, count, ?, u64 *out); 0x917630 CreateVertexBuffer(r, handle, size, usage, init);
//   0x9179d0 CreateIndexBuffer(r, handle, count, indexSize, usage, init); 0x91db40 / 0x91d890 destroy vertex / index buffer (r, handle)
//   0x922530 CreateBlendState, 0x922d10 CreateDepthStencilState (8 arguments: one on the stack), 0x923360 CreateRasterState -> handle
//   0x9279a0 SetRenderTargets(r, n, u64 color[], layers, mips, u64 depth): the set of handles is counted (~65 calls a frame)
//   engine opcode 55 (resource transition; 0x20-byte packet: +1 state, +8 texture handle, +0x10 const char *name or NULL): a handler in
//   the shared ExecuteRenderCommand chain (dn_install_rc), not a hook of its own.
// NR-06 (DM_NR_BUF 1 / 2 / 3, only with DM_NR=1 / 2; docs/dos2/nr/NR-06.md): the vertex / index buffer hooks also hand each engine buffer
//   (handle, guest range = the NVN buffer the call initialised, class static / dynamic / no-init; in mode 2 a hash of its bytes right
//   after creation) and each destroy to the engine (xbg_nr_buf_event: queued there, applied on the translation thread once per
//   submission); SetRenderTargets (every 64th call until found) and the 10 s census read the constant ring of the device
//   (dev = [r+0xb70]: GPU base [dev+0x25aa0], CPU base [dev+0x25a98] - equal in this HLE, GPU addresses are host pointers -, head
//   [dev+0x25a60]) and hand it over (xbg_nr_ring_set). With DM_NR_BUF unset / 0 none of this runs.
// Arguments and return values. Several of these take stack arguments, so every hook is ONE C shape: 8 integer registers, 8 FP registers
// and 12 stack slots in, the same 28 values passed on to the trampoline (the compiler rebuilds x0-x7, d0-d7 and the 12 outgoing slots;
// reading 12 slots of the caller's frame is always inside its stack), x0 returned. So the original sees unchanged arguments and the hook
// has a real `post` with the return value: no register-saving thunk, no swapped LR, no shadow stack. The hooks never tail-call the
// trampoline (NR_HOOK), so nothing is ever written into the caller's frame. Not carried: x8 (indirect result)
// and the upper halves of v0-v7 - tools/dos2_nr_check.py proves no hooked function reads x8 or a vector register before writing it.
// Threads: creation runs on several threads. The registry has ONE mutex (g_nr_mu) around every registry call; the brackets are
// thread-local (the NVN tap takes no lock); counters outside the registry are atomics. Nothing allocates after install except the dump.
// Guest memory is read through the kernel (process_vm_readv, and what it refuses through a pipe - nr_peek.h, the VM_PFNMAP dma-buf pages
// of the zero-copy pools: a bad pointer is an error return, never a fault; shader code included, NR-01 fix); the two hot paths
// (SetRenderTargets' handle array, the opcode-55 packet) read what the engine itself reads in the next instructions.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "../gfx/nr/nr_reg.h"
#include "../gfx/nr/nr_key.h"
#include "../gfx/nr/nr_tex.h"
#include "../gfx/nr/nr_buf.h"
#include "../gfx/nr/nr_sh.h"     // NR-04: the engine shaders by name (cross-check of names and keys)
#include "../gfx/nr/nr_peek.h"   // NR-01 fix: nr_pipe_read, the read process_vm_readv refuses (dma-buf pages of the zero-copy pools)
#include "../gfx/nr/nr_mem.h"    // NR-29: DM_NR_MEM, the RAM budget (dm_nr_mem_cap, hle_dos2_nr_mem.c)
uint64_t dm_nr_mem_cap(int which, uint64_t dflt);   // hle_dos2_nr_mem.c: a cache default -> what it may use (never more)
#include "../gfx/nvn/nvn_nr_tap.h"

// hle_dos2_native.c
typedef struct dn_rc_handler { void (*pre)(uint64_t list, unsigned op, uint64_t pkt); void (*post)(uint64_t list, unsigned op, uint64_t pkt, uint64_t len); } dn_rc_handler;
module_t *dn_main(void);
int dn_words_ok(module_t *m, uint32_t off, const uint32_t pro[4]);
int dn_hook(module_t *m, uint32_t off, const uint32_t pro[4], void *hook, void **orig);
int dn_install_rc(const dn_rc_handler *h);
uint64_t dm_nvn_frame_count(void);                                                   // hle_nvn.c
int nvn_pool_flags(const void *addr, size_t len);                                    // hle_nvn.c: flags of the pool holding the range, -1 none
// xbg_shader_key.c: DOS2's shader pack key. DOS2 builds with DM_GAME_D3, so xbg_game.cpp load_shader keys a stage by
// xbg_shader_program_key (control + code; NR-02 fix: NR-01 used xbg_shader_control_key, the key of the other games, which names no DOS2 bundle)
int xbg_shader_program_key(const void *control, const void *code, char out_hex17[17]) __attribute__((weak));
// NR-01 fix: the same key with the code read through a reader (nr_peek), never by a load of the key function
int xbg_shader_program_key_rd(const void *control, uint64_t code, int (*rd)(void *dst, uint64_t address, size_t n), char out_hex17[17]) __attribute__((weak));
// NR-06 (xbg_nr_buf.inc): ev 1 = created (kind NRS_KIND_*, guest range [mem, mem + size), class NRS_*, hash or 0), 2 = destroyed (kind, handle)
void xbg_nr_buf_event(int ev, int kind, uint64_t handle, uint64_t mem, uint64_t size, int cls, uint64_t hash) __attribute__((weak));
void xbg_nr_ring_set(uint64_t base, uint64_t size, uint64_t head_addr) __attribute__((weak));

int g_dm_nr;   // DM_NR: 0 off, 1 registry + census, 2 + checks. The NVN tap is live while it is non-zero.
static int g_nr_buf;           // DM_NR_BUF (NR-06): 0 off, 1..3 buffer events + the ring to the engine (set by the install only)
static uint64_t g_nr_ring;     // the ring base handed over (0 = not found yet)
static void nr_ring_probe(uint64_t r);
static nr_reg *g_reg;
static pthread_mutex_t g_nr_mu = PTHREAD_MUTEX_INITIALIZER, g_nr_rep_mu = PTHREAD_MUTEX_INITIALIZER;
static module_t *g_nr_main;
static int g_nr_dump, g_nr_dumps;
static struct {   // atomics (relaxed)
    uint64_t un_tex, un_prog, un_smp, un_buf;    // NVN objects initialised outside every engine bracket
    uint64_t deep, multi_stage, peek_fail, tick, code_unreadable;
    uint64_t checked, bad, bad_bit[NR_BAD_BITS], bad_lines;
    uint64_t rt_new, tr_new, tr_unnamed, get_hit, get_miss;
    uint64_t buf_ev, buf_nomem, ring_probes, srt_calls;   // NR-06
} g_nr;
static struct { uint64_t t0, frames, rt_calls, tr_calls, rt_new, tr_new, reports; } g_nr_win;   // the report window (g_nr_rep_mu)
#define NR_INC(x) __atomic_fetch_add(&(x), 1, __ATOMIC_RELAXED)
#define NR_GET(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

// NR-02 (hle_dos2_nr_key.inc): DM_NR_KEY, the draw identity front end; these are no-ops while it is 0
static int g_nr_key;
// NR-07 (DM_NR_PIPE 1..3, docs/dos2/nr/NR-07.md): the pack key of every program the game sets up goes to the backend's warm-up as a hint
// (xbg_game.cpp xbg_nr_pipe.inc: a warm record whose every stage was named is built first). 0: nothing.
static int g_nr_pipe;
static uint64_t g_nr_hint_sent, g_nr_hint_unkeyed;   // atomics
void xbg_nr_pipe_hint(const char *key, unsigned len) __attribute__((weak));
void xbg_nr_pipe_source(int on) __attribute__((weak));
static void nk_texture(uint64_t handle, uint64_t nvn_obj, unsigned views);
// NR-23 (hle_dos2_nr_nocall.inc): DM_NR_NOCALL; 0 = off (the flush hook is nr_h_tf, nothing else runs)
static int g_nr_nocall;
static void *nc_tf_hook(void *dflt);
static void nc_report(uint64_t frames);
// NR-09R (hle_dos2_nr_direct.inc): DM_NR_DIRECT; 0 = off. g_nrd_lean: the lean identity (1 on, 2 compared with nk_ident).
static int g_nrd, g_nrd_lean;
// NR-UBD (hle_dos2_nr_direct.inc nrub_install): DM_NR_UBD; 0 = off (every BindUniformBuffer of a direct draw is sent)
static int g_nrd_ubd;
static void nrd_ident_lr(uint64_t r, uint64_t lr);
static void nrd_ident_cmp(uint64_t r, uint64_t lr);
static void nrd_report(uint64_t frames);
static int nrn_on(void); static void nrn_rc55(uint64_t handle, uint64_t name); static void nrn_report(uint64_t frames);   // NR-NATIVE (hle_dos2_nrn.inc)
static void nrnev_report(uint64_t frames);   // [DOS2] NR-NATIVE step E (hle_dos2_nrn_ev.inc)
void dm_nrnd_tex_epoch(void);   // [DOS2] NR-NATIVE step D (hle_dos2_nrn_d.inc): a texture record was made / died
// NR-09S (hle_dos2_nr_state.inc): DM_NR_STATE; 0 = off (no native opcode handler)
static int g_nrs;
static void nrs_report(uint64_t frames);
// NR-09U (hle_dos2_nr_ubo.inc): DM_NR_UBO; 0 = off (the direct draw calls the engine's uniform-block functions)
static int g_nru;
static void nru_report(uint64_t frames);
static void nk_report(uint64_t now, uint64_t frames, int dump);

static uint64_t nr_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// ---- guest memory through the kernel ----
// process_vm_readv first; what it refuses is read again through a pipe of our own (nr_peek.h nr_pipe_read: copy_from_user, which
// reads the VM_PFNMAP dma-buf pages the zero-copy pools put over guest memory, and still returns EFAULT, never a signal, for an
// unmapped / PROT_NONE page). First phone run of NR-01 (nr1_r1, Mi 14 Ultra, DM_GFX_ZEROCOPY default 1): all 1066 program code areas
// lie in such pools, process_vm_readv refused every one ("unreadable 1066 ... programs with unreadable code 1066"), so no pack key.
// One pipe, created at the first refused read, under its own leaf mutex; a pipe that failed itself is closed and made again.
static pthread_mutex_t g_nr_pipe_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_nr_pfd[2] = {-1, -1};
static uint64_t g_nr_peek_pipe, g_nr_peek_refused;   // atomics: reads the pipe did after a refusal / refusals (both routes failed: peek_fail)
static int nr_peek_pipe(void *dst, uint64_t addr, size_t n) {
    pthread_mutex_lock(&g_nr_pipe_mu);
    if (g_nr_pfd[0] < 0) nr_pipe_open(g_nr_pfd);
    int r = g_nr_pfd[0] >= 0 ? nr_pipe_read(g_nr_pfd, dst, addr, n) : 0;
    if (r < 0) { close(g_nr_pfd[0]); close(g_nr_pfd[1]); g_nr_pfd[0] = g_nr_pfd[1] = -1; r = 0; }
    pthread_mutex_unlock(&g_nr_pipe_mu);
    return r;
}
static int nr_peek(void *dst, uint64_t addr, size_t n) {
    if (addr >= 0x10000 && addr < (1ull << 48) && n <= (1ull << 48) - addr) {
        struct iovec l = {dst, n}, r = {(void *)(uintptr_t)addr, n};
        const long got = syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL);
        if (got == (long)n) return 1;
        const int err = got < 0 ? errno : 0;
        if (!NR_GET(g_nr_peek_refused) && !__atomic_fetch_add(&g_nr_peek_refused, 1, __ATOMIC_RELAXED)) {
            // the first refusal says what it was (proof of the cause on the phone: errno 14 = EFAULT, a pool range, read by the pipe)
            uint8_t b = 0; const int pipe_ok = nr_peek_pipe(&b, addr, 1);
            LOG("[nr] guest read: process_vm_readv refused %#llx+%#zx (returned %ld, errno %d); one byte through a pipe (copy_from_user): %s; "
                "NVN pool flags of the range %d (-1 = no pool) - refused reads go through the pipe from now on",
                (unsigned long long)addr, n, got, err, pipe_ok ? "read" : "unreadable too", nvn_pool_flags((const void *)(uintptr_t)addr, n));
        } else NR_INC(g_nr_peek_refused);
        if (nr_peek_pipe(dst, addr, n)) { NR_INC(g_nr_peek_pipe); return 1; }
    }
    NR_INC(g_nr.peek_fail);
    return 0;
}
static uint64_t nr_peek64(uint64_t addr) { uint64_t v = 0; return nr_peek(&v, addr, 8) ? v : 0; }
// A text of at most cap - 1 characters at addr (len < 0: up to the first NUL), "" when unreadable.
static void nr_peek_text(char *dst, size_t cap, uint64_t addr, long len) {
    dst[0] = 0;
    if (!addr || !cap) return;
    size_t want = len < 0 || (size_t)len > cap - 1 ? cap - 1 : (size_t)len, got = 0;
    while (got < want) {   // page by page: a string that ends before an unmapped page still reads
        const size_t room = 0x1000 - (size_t)((addr + got) & 0xfff), n = want - got < room ? want - got : room;
        if (!nr_peek(dst + got, addr + got, n)) break;
        got += n;
        if (len < 0 && memchr(dst, 0, got)) break;
    }
    dst[got] = 0;
}
// The engine's handle tables: r + tab = {chunk pointer array, generation chunk array, u32 entries per chunk}; the record of `h` or 0.
static uint64_t nr_record(uint64_t r, uint32_t tab, uint32_t rec, uint64_t h) {
    uint8_t hd[0x14]; uint64_t chunks, gens; uint32_t per, gen = 0;
    if (!r || !nr_h_gen(h) || !nr_peek(hd, r + tab, sizeof hd)) return 0;
    memcpy(&chunks, hd, 8); memcpy(&gens, hd + 8, 8); memcpy(&per, hd + 0x10, 4);
    if (!chunks || !gens || !per) return 0;
    const uint32_t c = nr_h_idx(h) / per, i = nr_h_idx(h) % per;
    const uint64_t gc = nr_peek64(gens + (uint64_t)c * 8);
    if (!gc || !nr_peek(&gen, gc + (uint64_t)i * 4, 4) || gen != nr_h_gen(h)) return 0;
    const uint64_t rc = nr_peek64(chunks + (uint64_t)c * 8);
    return rc ? rc + (uint64_t)i * rec : 0;
}

// ---- the engine call bracket (thread-local) ----
enum { NRB_TEX = 1, NRB_IMAGE, NRB_WINDOW, NRB_SAMPLER, NRB_GET, NRB_LOAD, NRB_SHADER, NRB_VB, NRB_IB, NRB_DEPTH = 6, NRB_PROGS = 4 };
typedef struct nr_bracket {
    int kind, ntex, nprog, nsmp, nbuf, inner;   // inner: a CreateShader ran inside this Get / Load
    nr_nvn_tex tex[2]; nr_nvn_smp smp; uint64_t prog[NRB_PROGS], buf, buf_size, buf_mem;
    struct { uint64_t serial; const uint8_t *mem; nvn_texdesc d; nvn_texlayout L; } ts[2];   // NR-05: serial, storage, layout of the NVN textures
    char key[NRB_PROGS][17];
    char text[NR_PATH];                         // Get: the name, Load: the path
} nr_bracket;
static __thread struct { int depth; nr_bracket b[NRB_DEPTH]; } t_nr;
static nr_bracket *nr_open(int kind) {   // NULL past NRB_DEPTH (counted); nr_close balances either way
    const int d = t_nr.depth++;
    if (d >= NRB_DEPTH) { NR_INC(g_nr.deep); return NULL; }
    nr_bracket *b = &t_nr.b[d];
    b->kind = kind; b->ntex = b->nprog = b->nsmp = b->nbuf = b->inner = 0; b->buf = b->buf_size = b->buf_mem = 0; b->text[0] = 0;
    return b;
}
static void nr_close(void) { if (t_nr.depth > 0) t_nr.depth--; }
static nr_bracket *nr_find(unsigned mask) {   // innermost open bracket of one of the kinds
    for (int i = (t_nr.depth < NRB_DEPTH ? t_nr.depth : NRB_DEPTH) - 1; i >= 0; i--) if (mask >> t_nr.b[i].kind & 1) return &t_nr.b[i];
    return NULL;
}

// ---- the NVN tap (hle_nvn.c, any thread; only called while g_dm_nr != 0) ----
void dm_nr_tap_texture(const void *tex, int target, int format, int flags, int samples, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int packaged, uint64_t size,
                       uint64_t serial, const void *mem, const nvn_texdesc *desc, const nvn_texlayout *layout) {
    nr_bracket *b = nr_find(1u << NRB_TEX | 1u << NRB_IMAGE | 1u << NRB_WINDOW);
    if (!b) { NR_INC(g_nr.un_tex); return; }
    if (b->ntex < 2) {
        b->tex[b->ntex] = (nr_nvn_tex){.obj = (uint64_t)(uintptr_t)tex, .size = size, .w = w, .h = h, .d = d, .levels = levels, .target = target,
                                       .format = format, .flags = flags, .samples = samples, .packaged = packaged != 0};
        b->ts[b->ntex].serial = serial; b->ts[b->ntex].mem = (const uint8_t *)mem; b->ts[b->ntex].d = *desc; b->ts[b->ntex].L = *layout;
    }
    b->ntex++;
}
// The shader pack key of a stage, as xbg_game.cpp load_shader computes it from the same two pointers (control copy + code): the same
// SHA-256 (xbg_shader_key.c), but every byte of the code is read through nr_peek (4 KiB pieces), never by a load, so a code area that
// is not readable is no key (counted), not a fault. The code lives in a GPU pool, on Adreno a zero-copy one (dma-buf pages): before the
// NR-01 fix only process_vm_readv was tried, which refuses those pages, so every program was unkeyed.
static int nr_pack_key(const void *control, const void *code, char out[17]) {   // 1 = keyed
    out[0] = 0;
    if (!control || !xbg_shader_program_key_rd) return 0;
    if (xbg_shader_program_key_rd(control, (uint64_t)(uintptr_t)code, nr_peek, out)) { out[0] = 0; NR_INC(g_nr.code_unreadable); return 0; }
    return 1;
}
// NR-07: the key of a program just set up -> the warm-up (key = NULL: computed here, e.g. a program outside every engine call: UI)
static void nr_pipe_hint(const void *control0, const void *code0, const char *key) {
    char k[17];
    if (!key) { k[0] = 0; nr_pack_key(control0, code0, k); key = k; }
    if (!key[0]) { NR_INC(g_nr_hint_unkeyed); return; }
    xbg_nr_pipe_hint(key, 16); NR_INC(g_nr_hint_sent);
}
void dm_nr_tap_program(const void *prog, int nstage, const void *control0, const void *code0) {
    nr_bracket *b = nr_find(1u << NRB_SHADER);
    if (!b) { NR_INC(g_nr.un_prog); if (g_nr_pipe) nr_pipe_hint(control0, code0, NULL); return; }
    if (nstage != 1) NR_INC(g_nr.multi_stage);   // DOS2 gives each stage its own program
    if (b->nprog < NRB_PROGS) {
        b->prog[b->nprog] = (uint64_t)(uintptr_t)prog; b->key[b->nprog][0] = 0;
        nr_pack_key(control0, code0, b->key[b->nprog]);
        if (g_nr_pipe) nr_pipe_hint(NULL, NULL, b->key[b->nprog]);
    } else if (g_nr_pipe) nr_pipe_hint(control0, code0, NULL);
    b->nprog++;
}
void dm_nr_tap_sampler(const void *smp, unsigned minf, unsigned magf, unsigned ws, unsigned wt, unsigned wr, unsigned cmp_mode, unsigned cmp_func, float aniso) {
    nr_bracket *b = nr_find(1u << NRB_SAMPLER);
    if (!b) { NR_INC(g_nr.un_smp); return; }
    if (!b->nsmp) b->smp = (nr_nvn_smp){.obj = (uint64_t)(uintptr_t)smp, .minf = (uint8_t)minf, .magf = (uint8_t)magf, .ws = (uint8_t)ws, .wt = (uint8_t)wt,
                                        .wr = (uint8_t)wr, .cmp_mode = (uint8_t)cmp_mode, .cmp_func = (uint8_t)cmp_func, .have = 1, .aniso = aniso};
    b->nsmp++;
}
void dm_nr_tap_buffer(const void *buf, uint64_t size, const void *mem) {
    nr_bracket *b = nr_find(1u << NRB_VB | 1u << NRB_IB);
    if (!b) { NR_INC(g_nr.un_buf); return; }
    if (!b->nbuf) { b->buf = (uint64_t)(uintptr_t)buf; b->buf_size = size; b->buf_mem = (uint64_t)(uintptr_t)mem; }
    b->nbuf++;
}

// ---- NR-05: material textures by engine events (DM_NR_TEX; core dm/gfx/nr/nr_tex.h, engine side xbg_game.cpp namespace nrtex) ----
//   DM_NR_TEX=0 / unset  nothing (default): no table, no worker; the engine reads g_dm_nr_tex == 0 once and asks nothing.
//   1  on: at the end of CreateTextureFromImage (packaged or linear image data: the storage is complete) the texture's NVN serial is
//      recorded and a job queued; one worker thread (nice 10) deswizzles the storage into a host blob in the engine's staging layout and,
//      when the ASTC texture cache is active, takes its key. The engine copies the blob at the first upload (no deswizzle on the
//      translation thread), skips the full hash after it, never write-protects / hashes these textures again (tex-dirty, stable checks)
//      and checks a 33-sample probe instead at the old cadence; a changed probe re-uploads (a late write is never lost). A
//      WriteTexels into it (an engine event, exact) re-uploads it at its next check.
//   2  verify: the old path runs unchanged; the event also takes a full hash of the storage, the worker still prepares; the engine
//      compares create-event hash -> first upload, blob -> the old path's staging bytes, worker key -> its own, and reports every
//      re-upload of a live material texture (memory written after creation) and whether the probe saw it. Red lines, first 16.
//   3  keep: as 1, but a changed probe does NOT re-upload (pure "upload once"): the red lines show what that would leave stale.
//   DM_NR_TEX_PREP_MB   blob budget (default RAM / 64, 32..128 MB); a job waits while it is used up. DM_NR_TEX_MIN_KB (16): smaller
//   textures get no blob (the deswizzle costs less than the job). DM_NR_TEX_AGE_S (30): an untaken blob is freed after that long.
// NR-22 DM_NR_TEXSTREAM (with DM_NR_TEX 1 / 3; ETR PERF-E11 TEXSTREAM, docs/dos2/nr/NR-22.md):
//   0 / unset  NR-05 as above (heap blobs, the translator creates every VkImage).
//   1  the worker asks the engine (xbg_game.cpp namespace nrts, the sink below) for each job: for a format the translator already took
//      as it is, the engine creates the VkImage + device memory and hands out mapped host-visible staging; the worker deswizzles straight
//      into it. For a texture whose ASTC cache entry exists (Mali / PowerVR / Xclipse: BC1-BC3), the engine creates the ASTC image and
//      reads the entry's blocks into staging (xbg_ac_read_staged, NR-18). The translator, at the first upload, takes the object, checks
//      that its own VkImageCreateInfo is the prepared one and only records the copy (no vkCreateImage / vkAllocateMemory / deswizzle /
//      pread on the translation thread). Refused / not proven / mismatched -> the NR-05 or the old path, per texture.
//   2  verify: the worker prepares as in 1, the translator runs the old path (its own image, deswizzle / cache read) and compares the
//      create info, the memory requirements and the staged bytes with the prepared object (red lines), then drops the object.
//   DM_NR_TEXSTREAM_MB  prepared, not yet taken images + staging the engine may hold (default 2 x the blob budget; at most 1024 objects).
//   With DM_NR_TEXSTREAM, DM_NR_TEX_MIN_KB defaults to 0 (the cost moved is per image, small textures included).
int g_dm_nr_tex;   // read by the engine (weak)
// NR-05 fix: 1 once dm_dos2_nr_install has returned (whatever it decided, DM_NR_TEX 0 included; release). The engine's device init
// (xbg_game.cpp nrtex::init, the Vulkan thread) runs in parallel with host_preload and came ~190 ms BEFORE nrt_start on every phone run
// (nr16_tex1 / ts1 / ts2: "dirty tracker" line 22:57:02.807, "[nr] DM_NR_TEX=1" 22:57:03.002): it read g_dm_nr_tex == 0 and stayed
// off for good - the worker prepared every blob, nothing ever asked for one (keys 0, taken 0). The engine now waits for this flag.
int g_dm_nr_tex_set;
static uint64_t g_nrt_asks;   // atomic: dm_nr_tex_find calls (the engine's on_create of every new image); 0 with events = engine side off
int g_dm_nr_texstream;   // NR-22: read by the engine (weak); set only by nrt_start
uint64_t g_dm_nr_texstream_cap;   // NR-22: bytes of prepared objects the engine may hold
static nr_tex_core *g_nrt;
static pthread_mutex_t g_nrt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_nrt_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_nrt_age = 30000000000ull;
static nrt_job g_nrt_job;   // the worker's (one worker)
// The ASTC cache key of a storage (CRC-32 + Adler-32): the engine's own function (xbg_game.cpp, built with zlib for DOS2). Weak: no
// keys without it.
int xbg_astc_storage_key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) __attribute__((weak));
// NR-22: the engine's maker of prepared images (xbg_game.cpp, extern "C"; weak: no texstream without the engine). idle: the worker,
// between jobs and without the table lock, lets the engine destroy released objects.
void *xbg_nrts_make(void *ctx, const nrt_src *s, uint64_t bytes, uint8_t **dst) __attribute__((weak));
void *xbg_nrts_make_astc(void *ctx, const nrt_src *s, uint32_t crc, uint32_t adler, uint64_t *bytes) __attribute__((weak));
void xbg_nrts_release(void *ctx, void *obj) __attribute__((weak));
void xbg_nrts_idle(void) __attribute__((weak));
static void *nrt_worker(void *arg) {
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    pthread_mutex_lock(&g_nrt_mu);
    for (;;) {
        if (g_dm_nr_texstream) { pthread_mutex_unlock(&g_nrt_mu); xbg_nrts_idle(); pthread_mutex_lock(&g_nrt_mu); }   // NR-22: released objects die here
        if (!nr_tex_job_begin(g_nrt, &g_nrt_job)) {
            nr_tex_expire(g_nrt, nr_now(), g_nrt_age);
            struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 1;
            pthread_cond_timedwait(&g_nrt_cv, &g_nrt_mu, &t);
            continue;
        }
        pthread_mutex_unlock(&g_nrt_mu);
        const uint64_t t0 = nr_now();
        nr_tex_job_run(&g_nrt_job, xbg_astc_storage_key);
        g_nrt_job.ns = nr_now() - t0;
        pthread_mutex_lock(&g_nrt_mu);
        nr_tex_job_end(g_nrt, &g_nrt_job, nr_now());
    }
    return NULL;
}
// The create event (CreateTextureFromImage returned): the NVN texture its bracket saw.
static void nrt_event(uint64_t handle, int src, unsigned efmt, const nr_bracket *b) {
    if (!b || !b->ntex || !b->ts[0].serial) return;
    nrt_src s; memset(&s, 0, sizeof s);
    s.d = b->ts[0].d; s.L = b->ts[0].L; s.mem = b->ts[0].mem; s.serial = b->ts[0].serial; s.handle = handle; s.obj = b->tex[0].obj; s.src = (uint8_t)src; s.efmt = (uint8_t)efmt;
    const uint64_t hash0 = g_dm_nr_tex == NRT_VERIFY && s.mem ? nr_tex_hash(s.mem, s.L.size) : 0;   // outside the lock, on the engine's thread
    pthread_mutex_lock(&g_nrt_mu);
    const int why = nr_tex_create(g_nrt, &s, hash0, nr_now(), 1);
    pthread_mutex_unlock(&g_nrt_mu);
    if (why == NRT_WHY_OK) pthread_cond_signal(&g_nrt_cv);
}
static uint64_t g_nrt_wepoch;   // atomic: + 1 per WriteTexels into a live material texture (the engine re-asks only when it moved)
void dm_nr_tap_texture_written(uint64_t serial) {
    if (!g_dm_nr_tex) return;
    pthread_mutex_lock(&g_nrt_mu); const int live = nr_tex_written(g_nrt, serial); pthread_mutex_unlock(&g_nrt_mu);
    if (live) __atomic_fetch_add(&g_nrt_wepoch, 1, __ATOMIC_RELEASE);
}
uint64_t dm_nr_tex_write_epoch(void);
uint64_t dm_nr_tex_write_epoch(void) { return __atomic_load_n(&g_nrt_wepoch, __ATOMIC_ACQUIRE); }
void dm_nr_tap_texture_gone(uint64_t serial) {
    if (!g_dm_nr_tex) return;
    pthread_mutex_lock(&g_nrt_mu); nr_tex_gone(g_nrt, serial); pthread_mutex_unlock(&g_nrt_mu);
}
// The engine's side (xbg_game.cpp, translation thread; weak there). Each call takes the table lock briefly.
int dm_nr_tex_find(uint64_t serial, nrt_info *out);
int dm_nr_tex_find(uint64_t serial, nrt_info *out) {
    if (!g_dm_nr_tex) return 0;
    __atomic_fetch_add(&g_nrt_asks, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_nrt_mu); const int r = nr_tex_find(g_nrt, serial, out); pthread_mutex_unlock(&g_nrt_mu);
    return r;
}
void *dm_nr_tex_take(uint64_t serial, const void *mem, uint64_t size, uint64_t *bytes, uint64_t *probe, int *why);
void *dm_nr_tex_take(uint64_t serial, const void *mem, uint64_t size, uint64_t *bytes, uint64_t *probe, int *why) {
    if (!g_dm_nr_tex) { if (why) *why = NRT_WHY_UNKNOWN; return NULL; }
    pthread_mutex_lock(&g_nrt_mu); uint8_t *b = nr_tex_take(g_nrt, serial, mem, size, bytes, probe, why); pthread_mutex_unlock(&g_nrt_mu);
    return b;
}
void dm_nr_tex_drop(void *blob, uint64_t bytes);
void dm_nr_tex_drop(void *blob, uint64_t bytes) {
    if (!blob || !g_dm_nr_tex) return;
    pthread_mutex_lock(&g_nrt_mu); nr_tex_drop(g_nrt, (uint8_t *)blob, bytes); pthread_mutex_unlock(&g_nrt_mu);
    pthread_cond_signal(&g_nrt_cv);   // budget back: a waiting job may start
}
// NR-22: take with the engine's prepared object (see nr_tex_take_ts); unhold: the budget of a taken object back
int dm_nr_tex_take_ts(uint64_t serial, const void *mem, uint64_t size, uint8_t **blob, void **ts, int *kind, uint64_t *bytes, uint64_t *probe, int *why);
int dm_nr_tex_take_ts(uint64_t serial, const void *mem, uint64_t size, uint8_t **blob, void **ts, int *kind, uint64_t *bytes, uint64_t *probe, int *why) {
    if (!g_dm_nr_tex || !g_dm_nr_texstream) { if (why) *why = NRT_WHY_UNKNOWN; return 0; }
    pthread_mutex_lock(&g_nrt_mu); const int r = nr_tex_take_ts(g_nrt, serial, mem, size, blob, ts, kind, bytes, probe, why); pthread_mutex_unlock(&g_nrt_mu);
    return r;
}
void dm_nr_tex_unhold(uint64_t bytes);
void dm_nr_tex_unhold(uint64_t bytes) {
    if (!g_dm_nr_tex || !g_dm_nr_texstream) return;
    pthread_mutex_lock(&g_nrt_mu); nr_tex_unhold(g_nrt, bytes); pthread_mutex_unlock(&g_nrt_mu);
    pthread_cond_signal(&g_nrt_cv);
}
int dm_nr_tex_key(uint64_t serial, const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler);
int dm_nr_tex_key(uint64_t serial, const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) {
    if (!g_dm_nr_tex) return 0;
    pthread_mutex_lock(&g_nrt_mu); const int r = nr_tex_key(g_nrt, serial, mem, size, crc, adler); pthread_mutex_unlock(&g_nrt_mu);
    return r;
}
void dm_nr_tex_verdict(unsigned nvn_format, int ok);
void dm_nr_tex_verdict(unsigned nvn_format, int ok) {
    if (!g_dm_nr_tex) return;
    pthread_mutex_lock(&g_nrt_mu); nr_tex_verdict(g_nrt, nvn_format, ok); pthread_mutex_unlock(&g_nrt_mu);
}
void dm_nr_tex_want_key(int on);
void dm_nr_tex_want_key(int on) {
    if (!g_dm_nr_tex) return;
    pthread_mutex_lock(&g_nrt_mu); g_nrt->want_key = (uint8_t)(on != 0); pthread_mutex_unlock(&g_nrt_mu);
}
static int nrt_start(int mode, int ts) {   // install: the table and the worker; 0 = running. ts: DM_NR_TEXSTREAM (NR-22)
    const char *be = getenv("DM_NR_TEX_PREP_MB"), *me = getenv("DM_NR_TEX_MIN_KB"), *ae = getenv("DM_NR_TEX_AGE_S");
    const long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
    uint64_t mb = pages > 0 && psz > 0 ? ((uint64_t)pages * (uint64_t)psz / 64) >> 20 : 64;
    mb = mb < 32 ? 32 : mb > 128 ? 128 : mb;
    const uint64_t mb0 = mb;   // NR-29: the old default (the NR-22 default is 2 x it)
    if (be && atol(be) > 0) mb = (uint64_t)atol(be);
    else { mb = dm_nr_mem_cap(NRM_TEXBLOB, mb << 20) >> 20; if (!mb) mb = 1; }   // NR-29 DM_NR_MEM: the default by the RAM budget
    // NR-22: with texstream every size is prepared by default (vkCreateImage + memory cost per image, not per byte: the logs' 216-224 first
    // uploads of 3-10 MB cost 28-30 ms on the translator)
    const int ts_on = ts >= 1 && ts <= 2 && mode != NRT_VERIFY;
    const uint64_t budget = mb << 20, min = (uint64_t)(me ? atol(me) : ts_on ? 0 : 16) << 10;
    if (ae && atol(ae) > 0) g_nrt_age = (uint64_t)atol(ae) * 1000000000ull;
    g_nrt = nr_tex_new(budget, min, budget / 4);
    if (!g_nrt) return -1;
    if (ts_on) {   // NR-22: the engine's sink (before the worker and before guest code)
        if (xbg_nrts_make && xbg_nrts_make_astc && xbg_nrts_release && xbg_nrts_idle) {
            const nrt_sink sk = {xbg_nrts_make, xbg_nrts_make_astc, xbg_nrts_release, NULL};
            const char *ce = getenv("DM_NR_TEXSTREAM_MB");
            g_dm_nr_texstream_cap = ce && atol(ce) > 0 ? (uint64_t)atol(ce) << 20 : be && atol(be) > 0 ? 2 * budget : dm_nr_mem_cap(NRM_TEXSTREAM, 2 * (mb0 << 20));   // NR-29
            nr_tex_set_sink(g_nrt, &sk);
            g_dm_nr_texstream = ts;
            LOG("[nr] DM_NR_TEXSTREAM=%d (%s): the worker also makes the VkImage + memory and fills its own staging (ASTC cache entries read there too), "
                "up to %llu MB of prepared images + staging held (DM_NR_TEXSTREAM_MB)", ts, ts == 2 ? "verify: the old path runs, the prepared objects are compared and dropped" : "on",
                (unsigned long long)(g_dm_nr_texstream_cap >> 20));
        } else LOG("【红】[nr] DM_NR_TEXSTREAM=%d: the engine has no texstream maker (xbg_nrts_*): off", ts);
    } else if (ts) LOG("[nr] DM_NR_TEXSTREAM=%d ignored: it needs DM_NR_TEX=1 or 3 (DM_NR_TEX=2 verifies the old path)", ts);
    pthread_t th;
    if (pthread_create(&th, NULL, nrt_worker, NULL)) { nr_tex_free(g_nrt); g_nrt = NULL; return -1; }
    pthread_detach(th);
    g_dm_nr_tex = mode;   // before guest code runs
    LOG("[nr] DM_NR_TEX=%d (%s): material textures by engine events, blob budget %llu MB, min %llu KB, max %llu MB, untaken blobs freed after %llu s, one worker (nice 10)",
        mode, mode == NRT_VERIFY ? "verify: old path + create-event hash / blob / key / re-upload checks" : mode == NRT_KEEP ? "upload once, a changed probe is only reported" : "upload once, probe-guarded",
        (unsigned long long)mb, (unsigned long long)(min >> 10), (unsigned long long)((budget / 4) >> 20), (unsigned long long)(g_nrt_age / 1000000000ull));
    return 0;
}

// NR-29 DM_NR_MEM (hle_dos2_nr_mem.c): what the table holds (heap blobs + NR-22 staging of objects not taken), and a trim: every READY
// entry's blob / prepared object goes (as if expired: the translator takes the old path for it; NR-22 objects go to the engine's grave,
// destroyed by the worker, which is woken). Bytes freed. Any thread; nothing without NR-05.
uint64_t dm_nr_tex_mem_held(void);
uint64_t dm_nr_tex_mem_held(void) {
    if (!g_dm_nr_tex || !g_nrt) return 0;
    pthread_mutex_lock(&g_nrt_mu); const uint64_t b = g_nrt->st.live_bytes; pthread_mutex_unlock(&g_nrt_mu);
    return b;
}
uint64_t dm_nr_tex_mem_trim(void);
uint64_t dm_nr_tex_mem_trim(void) {
    if (!g_dm_nr_tex || !g_nrt) return 0;
    pthread_mutex_lock(&g_nrt_mu);
    const uint64_t b0 = g_nrt->st.live_bytes;
    nr_tex_expire(g_nrt, nr_now() + 1, 0);   // age 0: every READY one
    const uint64_t b1 = g_nrt->st.live_bytes;
    pthread_mutex_unlock(&g_nrt_mu);
    pthread_cond_signal(&g_nrt_cv);   // budget back + the worker destroys the released NR-22 objects (xbg_nrts_idle)
    return b0 > b1 ? b0 - b1 : 0;
}

// ---- checks (DM_NR=2) ----
static void nr_checked(const char *what, uint64_t handle, unsigned bad, const char *detail) {
    NR_INC(g_nr.checked);
    if (!bad) return;
    NR_INC(g_nr.bad);
    for (int b = 0; b < NR_BAD_BITS; b++) if (bad >> b & 1) NR_INC(g_nr.bad_bit[b]);
    if (__atomic_fetch_add(&g_nr.bad_lines, 1, __ATOMIC_RELAXED) < 16) {
        char why[128]; nr_bad_text(bad, why, sizeof why);
        LOG("【红】[nr] check: %s %#llx: %s - %s", what, (unsigned long long)handle, why, detail);
    }
}

// ---- the census (every 10 s, from whichever hook notices) and the dump ----
static void nr_dump(void) {   // g_nr_mu held
    const char *d = getenv("DM_NR_DUMP_DIR");
    if (!d || !*d) d = getenv("DM_RUNTIME_DIR");
    if (!d || !*d) d = "dm";
    int rows = 0;
    for (int w = 0; w < NR_TSV_N; w++) {
        char path[1100], tmp[1108];
        snprintf(path, sizeof path, "%s/%s", d, nr_tsv_name(w)); snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *o = fopen(tmp, "w");
        if (!o) { LOG("【红】[nr] dump: cannot write %s - dumps off", tmp); g_nr_dump = 0; return; }
        const int n = nr_write_tsv(g_reg, w, o);
        if (fclose(o) || rename(tmp, path)) { LOG("【红】[nr] dump: cannot finish %s - dumps off", path); g_nr_dump = 0; return; }
        rows += n > 0 ? n : 0;
    }
    if (++g_nr_dumps >= 64) g_nr_dump = 0;
    LOG("[nr] dump %d: %d files, %d rows in %s/nr_*.tsv%s", g_nr_dumps, NR_TSV_N, rows, d, g_nr_dump ? "" : " (64 dumps written - dumps off)");
}
static void nr_report(uint64_t now) {   // g_nr_rep_mu held
    static nr_census c;
    char sum[900];
    const uint64_t frames = dm_nvn_frame_count(), df = frames - g_nr_win.frames;
    pthread_mutex_lock(&g_nr_mu);
    nr_census_take(g_reg, &c); nr_summary(g_reg, sum, sizeof sum);
    const uint64_t rt_calls = g_reg->rtset_calls, tr_calls = g_reg->trans_calls, pool_calls = g_reg->pool_calls;
    const uint32_t nrt = g_reg->nrtset, ntr = g_reg->ntrans, npool = g_reg->npool;
    uint64_t kmiss[3] = {g_reg->st[NR_K_TEX].kill_miss, g_reg->st[NR_K_VB].kill_miss, g_reg->st[NR_K_IB].kill_miss};
    const uint64_t bkill = g_reg->st[NR_K_VB].kills + g_reg->st[NR_K_IB].kills, brep = g_reg->st[NR_K_VB].replaced + g_reg->st[NR_K_IB].replaced;
    const int dump_now = g_nr_dump && g_nr_win.reports % 3 == 0;
    if (dump_now) nr_dump();
    pthread_mutex_unlock(&g_nr_mu);
    if (g_nr_key) nk_report(now, df, dump_now);   // outside g_nr_mu: lock order key mutex -> registry mutex
    if (g_nrs) nrs_report(df);   // NR-09S
    nrn_report(df);   // NR-NATIVE
    nrnev_report(df);   // [DOS2] NR-NATIVE step E
    const double f = df ? (double)df : 1., mb = 1048576.;
    const uint64_t rt_new = NR_GET(g_nr.rt_new), tr_new = NR_GET(g_nr.tr_new);
    LOG("[nr] objects (DM_NR=%d, last %.1f s, %llu frames): %s; NVN objects outside an engine call: tex %llu prog %llu smp %llu buf %llu; unreadable %llu, nesting past %d: %llu, programs with unreadable code %llu; reads process_vm_readv refused %llu, of them read through the pipe %llu",
        g_dm_nr, (now - g_nr_win.t0) / 1e9, (unsigned long long)df, sum, (unsigned long long)NR_GET(g_nr.un_tex), (unsigned long long)NR_GET(g_nr.un_prog),
        (unsigned long long)NR_GET(g_nr.un_smp), (unsigned long long)NR_GET(g_nr.un_buf), (unsigned long long)NR_GET(g_nr.peek_fail), NRB_DEPTH, (unsigned long long)NR_GET(g_nr.deep),
        (unsigned long long)NR_GET(g_nr.code_unreadable), (unsigned long long)NR_GET(g_nr_peek_refused), (unsigned long long)NR_GET(g_nr_peek_pipe));
    const uint64_t img = c.tex_src_bytes[NR_SRC_IMAGE_LINEAR] + c.tex_src_bytes[NR_SRC_IMAGE_PACKAGED];
    LOG("[nr] textures by source: create %u (%.1f MB), create-data %u (%.1f MB), image-linear %u (%.1f MB), image-packaged %u (%.1f MB), rt %u (%.1f MB), window %u (%.1f MB); "
        "by format class: u8 %.1f MB, hdr %.1f MB, depth %.1f MB, bc %.1f MB, none %.1f MB; packaged share of image bytes %.1f%%; without an NVN texture %u; destroys not found %llu",
        c.tex_src[NR_SRC_CREATE], c.tex_src_bytes[NR_SRC_CREATE] / mb, c.tex_src[NR_SRC_CREATE_DATA], c.tex_src_bytes[NR_SRC_CREATE_DATA] / mb,
        c.tex_src[NR_SRC_IMAGE_LINEAR], c.tex_src_bytes[NR_SRC_IMAGE_LINEAR] / mb, c.tex_src[NR_SRC_IMAGE_PACKAGED], c.tex_src_bytes[NR_SRC_IMAGE_PACKAGED] / mb,
        c.tex_src[NR_SRC_RT], c.tex_src_bytes[NR_SRC_RT] / mb, c.tex_src[NR_SRC_WINDOW], c.tex_src_bytes[NR_SRC_WINDOW] / mb,
        c.tex_cls_bytes[NR_CLS_U8] / mb, c.tex_cls_bytes[NR_CLS_HDR] / mb, c.tex_cls_bytes[NR_CLS_DEPTH] / mb, c.tex_cls_bytes[NR_CLS_BC] / mb, c.tex_cls_bytes[NR_CLS_NONE] / mb,
        img ? 100. * c.tex_src_bytes[NR_SRC_IMAGE_PACKAGED] / img : 0., c.tex_no_nvn, (unsigned long long)kmiss[0]);
    LOG("[nr] buffers: vb static %u (%.1f MB) dynamic %u (%.1f MB), ib static %u (%.1f MB) dynamic %u (%.1f MB), destroyed %llu, replaced %llu, destroys not found %llu; "
        "frame: SetRenderTargets %.1f calls/frame, %u distinct sets (%llu new); transitions %.1f/frame, %u named keys (%llu new, %llu without a readable name); "
        "RT pool reservations %llu (%u keys); shader gets: %llu cached, %llu not a known handle",
        c.buf_n[0][0], c.buf_bytes[0][0] / mb, c.buf_n[0][1], c.buf_bytes[0][1] / mb, c.buf_n[1][0], c.buf_bytes[1][0] / mb, c.buf_n[1][1], c.buf_bytes[1][1] / mb,
        (unsigned long long)bkill, (unsigned long long)brep, (unsigned long long)(kmiss[1] + kmiss[2]),
        (rt_calls - g_nr_win.rt_calls) / f, nrt, (unsigned long long)(rt_new - g_nr_win.rt_new), (tr_calls - g_nr_win.tr_calls) / f, ntr,
        (unsigned long long)(tr_new - g_nr_win.tr_new), (unsigned long long)NR_GET(g_nr.tr_unnamed), (unsigned long long)pool_calls, npool,
        (unsigned long long)NR_GET(g_nr.get_hit), (unsigned long long)NR_GET(g_nr.get_miss));
    if (g_dm_nr_tex) {
        char t[1100];
        pthread_mutex_lock(&g_nrt_mu); nr_tex_summary(g_nrt, t, sizeof t); const uint64_t ev = g_nrt->st.events; pthread_mutex_unlock(&g_nrt_mu);
        const uint64_t asks = __atomic_load_n(&g_nrt_asks, __ATOMIC_RELAXED);
        LOG("[nr] textures by events (DM_NR_TEX=%d): %s; engine asks %llu", g_dm_nr_tex, t, (unsigned long long)asks);
        static int told;   // NR-05 fix: the failure of 17.11 (engine side never started) in one line instead of "taken 0"
        if (ev >= 64 && !asks && !told++) LOG("【红】[nr] textures by events: %llu events but the engine never asked for one (its NR-05 side is off: no \"NVN nr-tex DM_NR_TEX=\" line?)", (unsigned long long)ev);
    }
    if (g_nr_buf) {   // NR-06: the ring once more (moved?), and what went to the engine
        nr_ring_probe(nr_peek64((uint64_t)(uintptr_t)g_nr_main->base + 0x2bbdc68));
        LOG("[nr] buf (DM_NR_BUF=%d): ring %s%#llx, %llu buffer events to the engine, %llu buffers without an NVN buffer in their call, %llu ring probes",
            g_nr_buf, __atomic_load_n(&g_nr_ring, __ATOMIC_RELAXED) ? "at " : "not found yet ", (unsigned long long)__atomic_load_n(&g_nr_ring, __ATOMIC_RELAXED),
            (unsigned long long)NR_GET(g_nr.buf_ev), (unsigned long long)NR_GET(g_nr.buf_nomem), (unsigned long long)NR_GET(g_nr.ring_probes));
    }
    if (g_dm_nr == 2) {
        char by[400]; int k = 0; by[0] = 0;
        for (int b = 0; b < NR_BAD_BITS && k < (int)sizeof by - 40; b++) { const uint64_t n = NR_GET(g_nr.bad_bit[b]); if (n) k += snprintf(by + k, sizeof by - (size_t)k, " %s %llu", nr_bad_name(b), (unsigned long long)n); }
        LOG("[nr] checks (DM_NR=2): %llu objects checked, %llu mismatches%s%s", (unsigned long long)NR_GET(g_nr.checked), (unsigned long long)NR_GET(g_nr.bad), k ? ":" : "", by);
    }
    if (g_nr_pipe) LOG("[nr] pipe hints (DM_NR_PIPE=%d): %llu program keys sent to the warm-up, %llu programs without a key (unreadable code / no control)", g_nr_pipe,
                       (unsigned long long)NR_GET(g_nr_hint_sent), (unsigned long long)NR_GET(g_nr_hint_unkeyed));
    __atomic_store_n(&g_nr_win.t0, now, __ATOMIC_RELAXED); g_nr_win.frames = frames; g_nr_win.rt_calls = rt_calls; g_nr_win.tr_calls = tr_calls; g_nr_win.rt_new = rt_new; g_nr_win.tr_new = tr_new;
    g_nr_win.reports++;
}
static void nr_tick(void) {
    if (__atomic_fetch_add(&g_nr.tick, 1, __ATOMIC_RELAXED) & 31) return;
    const uint64_t now = nr_now();
    if (now - __atomic_load_n(&g_nr_win.t0, __ATOMIC_RELAXED) < 10000000000ull || pthread_mutex_trylock(&g_nr_rep_mu)) return;
    if (now - g_nr_win.t0 >= 10000000000ull) nr_report(now);
    pthread_mutex_unlock(&g_nr_rep_mu);
}

// ---- NR-06: buffers and the constant ring to the engine (DM_NR_BUF) ----
// The constant ring of the device (r = the renderer): handed over once found, and again if the device ever moved it.
static void nr_ring_probe(uint64_t r) {
    if (!g_nr_buf || !xbg_nr_ring_set || !r) return;
    NR_INC(g_nr.ring_probes);
    const uint64_t dev = nr_peek64(r + 0xb70);
    if (!dev) return;
    const uint64_t gpu = nr_peek64(dev + 0x25aa0), cpu = nr_peek64(dev + 0x25a98);
    if (!gpu || gpu == __atomic_load_n(&g_nr_ring, __ATOMIC_RELAXED)) return;
    if (cpu != gpu) {   // GPU addresses are host pointers here: the two bases must be one memory
        static int said;
        if (!said++) LOG("【红】[nr] buf: constant ring GPU base %#llx != CPU base %#llx (dev %#llx): ring mirror off", (unsigned long long)gpu, (unsigned long long)cpu, (unsigned long long)dev);
        return;
    }
    __atomic_store_n(&g_nr_ring, gpu, __ATOMIC_RELAXED);
    LOG("[nr] buf: constant ring at %#llx + %#x (dev %#llx, head at %#llx) handed to the engine", (unsigned long long)gpu, (unsigned)NR_RING_SIZE, (unsigned long long)dev,
        (unsigned long long)(dev + 0x25a60));
    xbg_nr_ring_set(gpu, NR_RING_SIZE, dev + 0x25a60);
}
// A vertex / index buffer the engine call created (b: its bracket; the NVN buffer the call initialised is its storage).
static void nr_buf_created(int kind, uint64_t handle, unsigned usage, int has_init, const nr_bracket *b) {
    if (!g_nr_buf || !xbg_nr_buf_event) return;
    if (!b || !b->nbuf || !b->buf_mem || !b->buf_size) { NR_INC(g_nr.buf_nomem); return; }
    const int cls = usage == NR_USAGE_DYNAMIC ? NRS_DYNAMIC : has_init ? NRS_ELIGIBLE : NRS_NOINIT;
    // mode 2: the bytes right after creation (HLE pool memory the tap just named: a host pointer of ours, readable)
    const uint64_t hash = g_nr_buf == 2 && cls != NRS_DYNAMIC ? nr_fnv64((const void *)(uintptr_t)b->buf_mem, (size_t)b->buf_size) | 1 : 0;
    NR_INC(g_nr.buf_ev);
    xbg_nr_buf_event(1, kind, handle, b->buf_mem, b->buf_size, cls, hash);
}
static void nr_buf_destroyed(int kind, uint64_t handle) {
    if (g_nr_buf && xbg_nr_buf_event) { NR_INC(g_nr.buf_ev); xbg_nr_buf_event(2, kind, handle, 0, 0, 0, 0); }
}

// ---- the hooks: one shape (see the header) ----
#define NR_A uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, \
             double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7, \
             uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11
#define NR_F a0, a1, a2, a3, a4, a5, a6, a7, d0, d1, d2, d3, d4, d5, d6, d7, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11
typedef uint64_t (*nr_fn)(NR_A);
// Never a tail call: a sibling call with stack arguments would write the 12 outgoing slots into the INCOMING argument area, i.e. into
// the caller's frame (slots the real caller never reserved as arguments). A plain call builds them in the hook's own frame.
#define NR_HOOK static __attribute__((disable_tail_calls)) uint64_t
static nr_fn g_o_ct, g_o_cti, g_o_win, g_o_dt, g_o_rsv, g_o_cs, g_o_sg, g_o_sl, g_o_sc, g_o_vf, g_o_vb, g_o_ib, g_o_dvb, g_o_dib, g_o_bs, g_o_ds, g_o_rs, g_o_srt;
static uint8_t nr_u8(uint64_t v) { return (uint32_t)v > 255 ? 255 : (uint8_t)v; }
#include "hle_dos2_nrn_ev.inc"   // [DOS2] NR-NATIVE step E: DM_NRN_EV (resources by engine events; nothing while it is 0)

// The texture the engine call made under `handle`: into the registry, and in mode 2 against the engine's own record (r+0: +0 the NVN
// texture object, +0x10 the engine format) and the expectations of nr_check_tex.
static void nr_tex_commit(uint64_t r, uint64_t handle, const nr_tex_desc *e, const nr_nvn_tex *nvn, int nnvn) {
    uint8_t rb[0x18]; int have = 0;
    if (g_dm_nr == 2 && r) { const uint64_t rec = nr_record(r, 0, 0x28, handle); have = rec && nr_peek(rb, rec, sizeof rb); }
    dm_nrnd_tex_epoch();   // [DOS2] NR-NATIVE step D: a texture record was (re)made - the key of the chain's memo moves
    pthread_mutex_lock(&g_nr_mu);
    nr_tex *t = nr_tex_put(g_reg, handle, e, nvn, nnvn);
    NRNEV_TEX(handle);   // [DOS2] NR-NATIVE step E
    nr_tex c = {0}; if (t && g_dm_nr == 2) { t->bad = nr_check_tex(t); c = *t; }
    pthread_mutex_unlock(&g_nr_mu);
    if (g_nr_key && t && nvn && nvn->obj) nk_texture(handle, nvn->obj, e->src == NR_SRC_CREATE || e->src == NR_SRC_CREATE_DATA || e->src == NR_SRC_RT ? (e->eflags & NR_TF_CUBE ? 6u : e->layers > 1 ? e->layers : 0u) : 0u);
    if (!t || g_dm_nr != 2) return;
    unsigned bad = c.bad;
    if (r) {
        uint64_t obj = 0; uint32_t fmt = 0;
        if (have) { memcpy(&obj, rb, 8); memcpy(&fmt, rb + 0x10, 4); }
        if (!have || obj != c.nvn.obj || fmt != c.efmt) bad |= NR_BAD_RECORD;
    }
    char d[320];
    const int image = c.src == NR_SRC_IMAGE_LINEAR || c.src == NR_SRC_IMAGE_PACKAGED;
    snprintf(d, sizeof d, "%s fmt %u (%s%s) %ux%ux%u mips %u layers %u msaa %u flags %#x/%#x override %d -> NVN %#llx fmt %#x (want %#x) target %d (want %d) %ux%ux%u levels %u samples %d packaged %u, %d NVN texture(s)",
             nr_src_name(c.src), c.efmt, nr_fmt_name(c.efmt), c.srgb ? ", sRGB" : "", c.w, c.h, c.d, c.mips, c.layers, c.msaa, c.eflags, c.eflags2, c.fmt_override,
             (unsigned long long)c.nvn.obj, (unsigned)c.nvn.format, (unsigned)nr_expect_nvn_format(c.efmt, c.srgb, c.fmt_override, image), c.nvn.target, nr_expect_target(&c),
             c.nvn.w, c.nvn.h, c.nvn.d, c.nvn.levels, c.nvn.samples, c.nvn.packaged, c.nnvn);
    nr_checked("texture", handle, bad, d);
}
NR_HOOK nr_h_ct(NR_A) {   // CreateTexture
    nr_bracket *b = nr_open(NRB_TEX);
    uint64_t data = 0;
    if (a2) nr_peek(&data, a2, 8);
    const uint64_t v = g_o_ct(NR_F);
    const uint16_t fl = (uint16_t)s3;
    const nr_tex_desc e = {.w = (uint32_t)a4, .h = (uint32_t)a5, .d = (uint32_t)a6, .fmt_override = (int32_t)s6, .eflags = fl, .eflags2 = (uint8_t)s4, .efmt = nr_u8(a3),
                           .mips = nr_u8(a7), .layers = nr_u8(s1), .msaa = nr_u8(s2), .src = (uint8_t)(fl & (NR_TF_COLOR_RT | NR_TF_DEPTH) ? NR_SRC_RT : data ? NR_SRC_CREATE_DATA : NR_SRC_CREATE),
                           .srgb = (uint8_t)(fl & NR_TF_SRGB)};
    nr_tex_commit(a0, a1, &e, b && b->ntex ? &b->tex[0] : NULL, b ? b->ntex : 0);
    nr_close(); nr_tick();
    return v;
}
NR_HOOK nr_h_cti(NR_A) {   // CreateTextureFromImage
    nr_bracket *b = nr_open(NRB_IMAGE);
    uint8_t im[0x48]; uint32_t efmt = 255, flags = 0; int32_t ov = -1, pk = -1; uint64_t w = 0, h = 0, d = 0, mips = 0, layers = 0;
    const int have = a2 && nr_peek(im, a2, sizeof im);   // before the call: the image may be gone after it
    if (have) { memcpy(&efmt, im, 4); memcpy(&flags, im + 4, 4); memcpy(&ov, im + 8, 4); memcpy(&pk, im + 0xc, 4); memcpy(&w, im + 0x18, 8); memcpy(&h, im + 0x20, 8);
                memcpy(&d, im + 0x28, 8); memcpy(&mips, im + 0x38, 8); memcpy(&layers, im + 0x40, 8); }
    const uint64_t v = g_o_cti(NR_F);
    const nr_tex_desc e = {.w = (uint32_t)w, .h = (uint32_t)h, .d = (uint32_t)d, .fmt_override = ov, .eflags = (uint16_t)((flags & NR_TF_CUBE) | 4), .eflags2 = 0, .efmt = nr_u8(efmt),
                           .mips = nr_u8(mips), .layers = nr_u8(layers), .msaa = 1, .src = (uint8_t)(pk == -1 ? NR_SRC_IMAGE_LINEAR : NR_SRC_IMAGE_PACKAGED), .srgb = (uint8_t)(a3 & 1)};
    nr_tex_commit(a0, a1, &e, b && b->ntex ? &b->tex[0] : NULL, b ? b->ntex : 0);
    if (g_dm_nr_tex && have) nrt_event(a1, e.src, efmt, b);   // NR-05: the storage is complete now
    nr_close(); nr_tick();
    return v;
}
NR_HOOK nr_h_win(NR_A) {   // the window's textures: the engine registers their handles itself (window +0x68, +0x70)
    nr_bracket *b = nr_open(NRB_WINDOW);
    const uint64_t v = g_o_win(NR_F);
    if (b && b->ntex) {
        uint64_t hd[2] = {0, 0}; uint32_t efmt = 255;
        nr_peek(hd, a0 + 0x68, sizeof hd); nr_peek(&efmt, a0 + 0x148, 4);
        for (int i = 0; i < b->ntex && i < 2; i++) {
            const nr_tex_desc e = {.w = b->tex[i].w, .h = b->tex[i].h, .d = 0, .fmt_override = 0, .eflags = 0, .eflags2 = 0, .efmt = nr_u8(efmt), .mips = 1, .layers = 1,
                                   .msaa = 1, .src = NR_SRC_WINDOW, .srgb = 0};
            nr_tex_commit(0, hd[i], &e, &b->tex[i], 1);
        }
    }
    nr_close(); nr_tick();
    return v;
}
NR_HOOK nr_h_dt(NR_A) {   // deferred texture destroy: x1 = a copy of the record; 1 = finalized now (0 = not due yet)
    const uint64_t obj = a1 ? nr_peek64(a1) : 0;
    if (g_dm_nr_tex && obj) { pthread_mutex_lock(&g_nrt_mu); nr_tex_engine_gone(g_nrt, obj); pthread_mutex_unlock(&g_nrt_mu); }   // NR-05: queued = engine-dead
    const uint64_t v = g_o_dt(NR_F);
    if (v & 0xff) dm_nrnd_tex_epoch();   // [DOS2] NR-NATIVE step D: a texture record died - the key of the chain's memo moves
    if ((v & 0xff) && obj) { pthread_mutex_lock(&g_nr_mu); nr_tex_kill_obj(g_reg, obj); pthread_mutex_unlock(&g_nr_mu); }
    return v;
}
NR_HOOK nr_h_rsv(NR_A) {   // shared RT storage reservation: no texture, no NVN object
    pthread_mutex_lock(&g_nr_mu); nr_pool_note(g_reg, a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4); pthread_mutex_unlock(&g_nr_mu);
    return g_o_rsv(NR_F);
}
NR_HOOK nr_h_cs(NR_A) {   // CreateSampler -> handle
    nr_bracket *b = nr_open(NRB_SAMPLER);
    const uint64_t v = g_o_cs(NR_F);
    pthread_mutex_lock(&g_nr_mu);
    nr_sampler *s = nr_sampler_put(g_reg, v, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint32_t)a5, (uint8_t)a6, b && b->nsmp ? &b->smp : NULL);
    nr_sampler c = {0}; if (s && g_dm_nr == 2) { s->bad = nr_check_sampler(s); c = *s; }
    pthread_mutex_unlock(&g_nr_mu);
    if (s && g_dm_nr == 2) {
        char d[200];
        snprintf(d, sizeof d, "filter %u wrap %u/%u/%u aniso %u compare %u -> NVN %#llx min %u mag %u wrap %u/%u/%u compare %u/%u aniso %g", c.filter, c.wrap[0], c.wrap[1], c.wrap[2],
                 c.aniso, c.compare, (unsigned long long)c.nvn.obj, c.nvn.minf, c.nvn.magf, c.nvn.ws, c.nvn.wt, c.nvn.wr, c.nvn.cmp_mode, c.nvn.cmp_func, (double)c.nvn.aniso);
        nr_checked("sampler", v, c.bad, d);
    }
    nr_close(); nr_tick();
    return v;
}
static void nr_state_commit(int kind, uint64_t handle, const uint8_t *arg, int n) {
    pthread_mutex_lock(&g_nr_mu);
    nr_state *s = nr_state_put(g_reg, kind, handle, arg, n);
    const unsigned bad = g_dm_nr == 2 ? nr_check_state(kind, arg, n) : 0;
    if (s) s->bad = bad;
    pthread_mutex_unlock(&g_nr_mu);
    if (s && g_dm_nr == 2) {
        char d[96]; int k = 0;
        for (int i = 0; i < n && k < (int)sizeof d - 8; i++) k += snprintf(d + k, sizeof d - (size_t)k, "%s%u", i ? "," : "args ", arg[i]);
        nr_checked(nr_state_name(kind), handle, bad, d);
    }
    nr_tick();
}
NR_HOOK nr_h_bs(NR_A) {   // CreateBlendState(r, srcColor, dstColor, srcAlpha, dstAlpha, op, separate) -> handle
    const uint64_t v = g_o_bs(NR_F);
    const uint8_t a[6] = {(uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint8_t)a5, (uint8_t)a6};
    nr_state_commit(NR_ST_BLEND, v, a, 6);
    return v;
}
NR_HOOK nr_h_ds(NR_A) {   // CreateDepthStencilState(r, depthTest, depthWrite, depthFunc, stencilTest, ref, stencilFunc, op; STACK: op) -> handle
    const uint64_t v = g_o_ds(NR_F);
    const uint8_t a[8] = {(uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint8_t)a5, (uint8_t)a6, (uint8_t)a7, (uint8_t)s0};
    nr_state_commit(NR_ST_DEPTH, v, a, 8);
    return v;
}
NR_HOOK nr_h_rs(NR_A) {   // CreateRasterState(r, cull, fill, flag, flag, polygonOffset) -> handle
    const uint64_t v = g_o_rs(NR_F);
    const uint8_t a[5] = {(uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4, (uint8_t)a5};
    nr_state_commit(NR_ST_RASTER, v, a, 5);
    return v;
}
NR_HOOK nr_h_vf(NR_A) {   // CreateVertexFormat(r, elements, count, ?, u64 *out)
    uint8_t el[NR_VF_ELEMS * 6]; const uint32_t count = (uint32_t)a2, kept = count > NR_VF_ELEMS ? NR_VF_ELEMS : count;
    const int have = kept && a1 && nr_peek(el, a1, (size_t)kept * 6);
    const uint64_t v = g_o_vf(NR_F);
    uint64_t h = 0;
    if (a4 && nr_peek(&h, a4, 8)) { pthread_mutex_lock(&g_nr_mu); nr_vformat_put(g_reg, h, have ? el : NULL, count); pthread_mutex_unlock(&g_nr_mu); }
    nr_tick();
    return v;
}
NR_HOOK nr_h_vb(NR_A) {   // CreateVertexBuffer(r, handle, size, usage, init)
    nr_bracket *b = nr_open(NRB_VB);
    const uint64_t v = g_o_vb(NR_F);
    pthread_mutex_lock(&g_nr_mu); nr_buffer_put(g_reg, NR_BUF_VERTEX, a1, a2, 0, 0, (uint8_t)a3, b ? b->buf : 0, b ? b->buf_size : 0); pthread_mutex_unlock(&g_nr_mu);
    nr_buf_created(NRS_KIND_VB, a1, (uint8_t)a3, a4 != 0, b);   // NR-06
    NRNEV_CREATED(NRS_KIND_VB, a1, (uint8_t)a3, a4 != 0, b);   // [DOS2] NR-NATIVE step E
    nr_close(); nr_tick();
    return v;
}
NR_HOOK nr_h_ib(NR_A) {   // CreateIndexBuffer(r, handle, count, indexSize, usage, init)
    nr_bracket *b = nr_open(NRB_IB);
    const uint64_t v = g_o_ib(NR_F);
    pthread_mutex_lock(&g_nr_mu);
    nr_buffer_put(g_reg, NR_BUF_INDEX, a1, (uint64_t)(uint32_t)a2 * (uint32_t)a3, (uint32_t)a2, nr_u8(a3), (uint8_t)a4, b ? b->buf : 0, b ? b->buf_size : 0);
    pthread_mutex_unlock(&g_nr_mu);
    nr_buf_created(NRS_KIND_IB, a1, (uint8_t)a4, a5 != 0, b);   // NR-06
    NRNEV_CREATED(NRS_KIND_IB, a1, (uint8_t)a4, a5 != 0, b);   // [DOS2] NR-NATIVE step E
    nr_close(); nr_tick();
    return v;
}
NR_HOOK nr_h_dvb(NR_A) { pthread_mutex_lock(&g_nr_mu); nr_buffer_kill(g_reg, NR_BUF_VERTEX, a1); pthread_mutex_unlock(&g_nr_mu); nr_buf_destroyed(NRS_KIND_VB, a1); NRNEV_DESTROYED(NRS_KIND_VB, a1); /* [DOS2] NR-NATIVE step E */ return g_o_dvb(NR_F); }
NR_HOOK nr_h_dib(NR_A) { pthread_mutex_lock(&g_nr_mu); nr_buffer_kill(g_reg, NR_BUF_INDEX, a1); pthread_mutex_unlock(&g_nr_mu); nr_buf_destroyed(NRS_KIND_IB, a1); NRNEV_DESTROYED(NRS_KIND_IB, a1); /* [DOS2] NR-NATIVE step E */ return g_o_dib(NR_F); }
NR_HOOK nr_h_sg(NR_A) {   // ShaderManager::Get(mgr, FixedString *name, flag): a cache hit creates nothing
    nr_bracket *b = nr_open(NRB_GET);
    if (b && a1) {
        const uint64_t chars = nr_peek64(a1); uint16_t len = 0;
        if (chars && nr_peek(&len, chars - 8, 2)) nr_peek_text(b->text, NR_NAME, chars, len);
    }
    const uint64_t v = g_o_sg(NR_F);
    if (b && !b->inner) {
        pthread_mutex_lock(&g_nr_mu); const int known = nr_shader_note_get(g_reg, v, b->text); pthread_mutex_unlock(&g_nr_mu);
        if (known) NR_INC(g_nr.get_hit); else NR_INC(g_nr.get_miss);
    }
    nr_close(); nr_tick();
    return v;
}
NR_HOOK nr_h_sl(NR_A) {   // LoadShaderFile(path, flag)
    nr_bracket *b = nr_open(NRB_LOAD);
    uint8_t s[16];
    if (b && a0 && nr_peek(s, a0, sizeof s)) {
        if (s[0] & 1) { uint32_t len; uint64_t p; memcpy(&len, s + 4, 4); memcpy(&p, s + 8, 8); nr_peek_text(b->text, NR_PATH, p, len); }
        else { const unsigned len = s[0] >> 1 < 15 ? s[0] >> 1 : 15; memcpy(b->text, s + 1, len); b->text[len] = 0; }
    }
    const uint64_t v = g_o_sl(NR_F);
    nr_close();
    return v;
}
// NR-04: a red line for every engine name / key the table of dm/gfx/nr/nr_sh.c does not agree with (first 16); agreement is counted there.
static void nr_sh_names(const char *name, const char *path, const char *fs_key) {
    static int reds;
    int idx = -1;
    const int r = nr_sh_name_note(name, path, fs_key, &idx);
    if (r >= 0 || __atomic_add_fetch(&reds, 1, __ATOMIC_RELAXED) > 16) return;
    LOG("【红】[nr] sh name check: engine shader '%s' (path '%s') fragment key %s, table entry %s (%s): %s", name ? name : "", path ? path : "",
        fs_key && *fs_key ? fs_key : "-", idx >= 0 ? nr_sh_table[idx].name : "-", idx >= 0 ? nr_sh_table[idx].key : "-",
        r == NR_SH_NAME_KEY_DIFFERS ? "the table's name with another code (that code is not replaced; ours is untouched)"
        : r == NR_SH_NAME_NAME_DIFFERS ? "our code under a name the table does not know (still replaced: the key and the head decide)"
        : "our code under the name of another entry: that entry is off, its translated shader is used from now on");
}
NR_HOOK nr_h_sc(NR_A) {   // CreateShader(r, vs, fs, cs) -> handle; the stage programs sit in the engine's record (r+0xc0, 0x60 each: +8 VS, +0x10 FS, +0x18 CS)
    nr_bracket *b = nr_open(NRB_SHADER);
    const uint64_t v = g_o_sc(NR_F);
    if (b) {
        const char *name = NULL, *path = NULL;
        for (int i = t_nr.depth - 2; i >= 0; i--) {   // the callers still open on this thread
            if (i >= NRB_DEPTH) continue;
            nr_bracket *o = &t_nr.b[i];
            if (o->kind == NRB_GET) { o->inner = 1; if (!name) name = o->text; }
            else if (o->kind == NRB_LOAD) { o->inner = 1; if (!path) path = o->text; }
            else break;
        }
        uint64_t prog[NR_STAGES] = {0, 0, 0}; char key[NR_STAGES][17] = {{0}, {0}, {0}};
        const uint64_t rec = nr_record(a0, 0xc0, 0x60, v);
        const int have = rec && nr_peek(prog, rec + 8, sizeof prog);
        unsigned bad = 0; int matched = 0;
        const int n = b->nprog < NRB_PROGS ? b->nprog : NRB_PROGS;
        if (have) {
            for (int k = 0; k < NR_STAGES; k++) for (int i = 0; i < n; i++) if (prog[k] && prog[k] == b->prog[i]) { memcpy(key[k], b->key[i], 17); matched++; break; }
            if (matched != n) bad |= NR_BAD_PROGRAM;   // a program the tap saw is not in the record
        } else {   // no record: creation order (vertex, fragment, compute); a lone program cannot be told apart
            bad |= NR_BAD_RECORD;
            for (int i = 0; i < n && i < NR_STAGES; i++) { prog[i] = b->prog[i]; memcpy(key[i], b->key[i], 17); }
        }
        pthread_mutex_lock(&g_nr_mu);
        nr_shader *s = nr_shader_put(g_reg, v, name, path, prog, (const char (*)[17])key);
        if (s && g_dm_nr == 2) { bad |= nr_check_shader(s); s->bad = bad; }
        pthread_mutex_unlock(&g_nr_mu);
        nr_sh_names(name, path, key[1]);   // NR-04 (DM_NR_SH): the engine's name of a shader against the table's
        if (s && g_dm_nr == 2) {
            char d[420];
            snprintf(d, sizeof d, "name '%s' path '%s' programs vs %#llx (%s) fs %#llx (%s) cs %#llx (%s), %d from the tap, %d in the record", name ? name : "", path ? path : "",
                     (unsigned long long)prog[0], key[0], (unsigned long long)prog[1], key[1], (unsigned long long)prog[2], key[2], b->nprog, matched);
            nr_checked("shader", v, bad, d);
        }
    }
    nr_close(); nr_tick();
    return v;
}
NR_HOOK nr_h_srt(NR_A) {   // SetRenderTargets(r, n, u64 color[], layers, mips, u64 depth): hot (~65 a frame), count only
    const uint32_t n = (uint32_t)a1;
    uint64_t col[NR_RT_COLORS] = {0};
    if (n && n <= NR_RT_COLORS && a2) memcpy(col, (const void *)(uintptr_t)a2, (size_t)n * 8);   // the array the engine reads next
    pthread_mutex_lock(&g_nr_mu);
    const int fresh = nr_rtset_note(g_reg, a2 ? n : 0, col, a5);
    pthread_mutex_unlock(&g_nr_mu);
    if (fresh == 1) NR_INC(g_nr.rt_new);
    if (g_nr_buf && !__atomic_load_n(&g_nr_ring, __ATOMIC_RELAXED) && !(__atomic_fetch_add(&g_nr.srt_calls, 1, __ATOMIC_RELAXED) & 63)) nr_ring_probe(a0);   // NR-06: until found
    nr_tick();
    return g_o_srt(NR_F);
}

// ---- engine opcode 55 (resource transition) through the shared ExecuteRenderCommand chain ----
static int g_nrnd; static void nrnd_pkt(unsigned op, uint64_t pkt);   // [DOS2] NR-NATIVE step D (hle_dos2_nrn_d.inc)
static void nr_rc_pre(uint64_t list, unsigned op, uint64_t pkt) {
    NRNEV_RC(list, op, pkt);   // [DOS2] NR-NATIVE step E: the opcodes that write a buffer / a texture, the ring's head
    (void)list;
    if (__builtin_expect(g_nrnd, 0)) nrnd_pkt(op, pkt);   // [DOS2] NR-NATIVE step D: every packet the thread executes (a packet outside a noted group breaks the chain)
    if (op != 55 || !pkt) return;
    const uint8_t *p = (const uint8_t *)(uintptr_t)pkt;   // the packet the engine's handler reads next
    uint64_t handle, name; memcpy(&handle, p + 8, 8); memcpy(&name, p + 0x10, 8);
    const unsigned state = p[1];
    if (__builtin_expect(nrn_on(), 0)) nrn_rc55(handle, name);   // NR-NATIVE: the class of the named target
    pthread_mutex_lock(&g_nr_mu);
    int fresh = nr_trans_find(g_reg, name, state) ? nr_trans_note(g_reg, name, NULL, handle, state) : 2;
    pthread_mutex_unlock(&g_nr_mu);
    if (fresh == 2) {   // a key not seen before: read its name once, outside the lock
        char text[64]; text[0] = 0;
        if (name) {
            const module_t *m = g_nr_main; const uint64_t lo = (uint64_t)(uintptr_t)m->base, hi = lo + m->size;
            if (name >= lo && name < hi) { size_t i = 0; const char *s = (const char *)(uintptr_t)name; while (i < sizeof text - 1 && name + i < hi && s[i]) { text[i] = s[i]; i++; } text[i] = 0; }
            else nr_peek_text(text, sizeof text, name, -1);
            if (!text[0]) NR_INC(g_nr.tr_unnamed);
        }
        pthread_mutex_lock(&g_nr_mu); fresh = nr_trans_note(g_reg, name, text, handle, state); pthread_mutex_unlock(&g_nr_mu);
        if (fresh == 1) NR_INC(g_nr.tr_new);
    }
    nr_tick();
}
static const dn_rc_handler k_nr_rc = {nr_rc_pre, NULL};
static const dn_rc_handler k_nr_rc_ev = {nr_rc_pre, nrnev_rc_post};   // [DOS2] NR-NATIVE step E: + after the CPU copies

// ---- install ----
static const struct nr_site { uint32_t off; uint32_t pro[4]; const char *name; void *hook; nr_fn *orig; } k_nr_site[] = {
    {0x91e2a0, {0xd105c3ff /* sub sp,sp,#0x170 */, 0xa9116ffc, 0xa91267fa, 0xa9135ff8}, "CreateTexture", (void *)nr_h_ct, &g_o_ct},
    {0x91f2e0, {0xd10503ff /* sub sp,sp,#0x140 */, 0xa90e6ffc, 0xa90f67fa, 0xa9105ff8}, "CreateTextureFromImage", (void *)nr_h_cti, &g_o_cti},
    {0x93c3a0, {0xd10403ff /* sub sp,sp,#0x100 */, 0xa90a6ffc, 0xa90b67fa, 0xa90c5ff8}, "window textures", (void *)nr_h_win, &g_o_win},
    {0x91d630, {0xf81e0ff3 /* str x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0xf9400429 /* ldr x9,[x1,#8] */}, "texture destroy", (void *)nr_h_dt, &g_o_dt},
    {0x914bb0, {0xd10383ff /* sub sp,sp,#0xe0 */, 0xf90053f7, 0xa90b57f6, 0xa90c4ff4}, "RT storage reservation", (void *)nr_h_rsv, &g_o_rsv},
    {0x921e40, {0xd103c3ff /* sub sp,sp,#0xf0 */, 0xa90a67fa, 0xa90b5ff8, 0xa90c57f6}, "CreateSampler", (void *)nr_h_cs, &g_o_cs},
    {0x8f8430, {0xd10103ff /* sub sp,sp,#0x40 */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}, "ShaderManager::Get", (void *)nr_h_sg, &g_o_sg},
    {0x92d320, {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6}, "LoadShaderFile", (void *)nr_h_sl, &g_o_sl},
    {0x915ad0, {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xa9056ffc, 0xa90667fa, 0xa9075ff8}, "CreateShader", (void *)nr_h_sc, &g_o_sc},
    {0x917100, {0xd10183ff /* sub sp,sp,#0x60 */, 0xa9025ff8, 0xa90357f6, 0xa9044ff4}, "CreateVertexFormat", (void *)nr_h_vf, &g_o_vf},
    {0x917630, {0xd10203ff /* sub sp,sp,#0x80 */, 0xf90023f7, 0xa90557f6, 0xa9064ff4}, "CreateVertexBuffer", (void *)nr_h_vb, &g_o_vb},
    {0x9179d0, {0xd10203ff /* sub sp,sp,#0x80 */, 0xf90023f7, 0xa90557f6, 0xa9064ff4}, "CreateIndexBuffer", (void *)nr_h_ib, &g_o_ib},
    {0x91db40, {0xf81b0ff9 /* str x25,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4}, "vertex buffer destroy", (void *)nr_h_dvb, &g_o_dvb},
    {0x91d890, {0xf81b0ff9 /* str x25,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4}, "index buffer destroy", (void *)nr_h_dib, &g_o_dib},
    {0x922530, {0xd10203ff /* sub sp,sp,#0x80 */, 0xa90367fa, 0xa9045ff8, 0xa90557f6}, "CreateBlendState", (void *)nr_h_bs, &g_o_bs},
    {0x922d10, {0xd10243ff /* sub sp,sp,#0x90 */, 0xa9036ffc, 0xa90467fa, 0xa9055ff8}, "CreateDepthStencilState", (void *)nr_h_ds, &g_o_ds},
    {0x923360, {0xd10203ff /* sub sp,sp,#0x80 */, 0xa901efff, 0xa90367fa, 0xa9045ff8}, "CreateRasterState", (void *)nr_h_rs, &g_o_rs},
    {0x9279a0, {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6}, "SetRenderTargets", (void *)nr_h_srt, &g_o_srt},
};
enum { NR_SITES = sizeof k_nr_site / sizeof k_nr_site[0] };

#include "hle_dos2_nrn.inc"   // NR-NATIVE: DM_NRN_<PASS> (the card of an engine draw; nothing while every pass is 0)
#include "hle_dos2_nrn_d.inc"   // [DOS2] NR-NATIVE step D: DM_NRN_LIST (the note of list construction; nothing while it is 0)
#include "hle_dos2_nr_key.inc"   // NR-02: DM_NR_KEY (draw identity; nothing while it is 0)
#include "hle_dos2_nr_nocall.inc"   // NR-23: DM_NR_NOCALL (the engine's own NVN calls at a draw; nothing while it is 0)
#include "hle_dos2_nr_ubo.inc"   // NR-09U: DM_NR_UBO (the uniform blocks of a direct draw as native code; nothing while it is 0)
#include "hle_dos2_nr_direct.inc"   // NR-09R: DM_NR_DIRECT (the engine draw recorded directly; nothing while it is 0)
#include "hle_dos2_nr_state.inc"   // NR-09S: DM_NR_STATE (opcode 21, all state of a draw, as native code; nothing while it is 0)

void dm_dos2_nr_install(void);   // main.c host_preload, after dm_dos2_nb_install (before any guest code runs)
static void nr_install_done(int *unused) { (void)unused; __atomic_store_n(&g_dm_nr_tex_set, 1, __ATOMIC_RELEASE); }   // NR-05 fix: every return below
void dm_dos2_nr_install(void) {
    int nr_done __attribute__((cleanup(nr_install_done))) = 0; (void)nr_done;   // NR-05 fix: g_dm_nr_tex_set = 1 when this returns (any path)
    const char *e = getenv("DM_NR"), *te = getenv("DM_NR_TEX"), *se = getenv("DM_NR_TEXSTREAM");
    const int asked = e ? atoi(e) : 0, ts = se ? atoi(se) : 0, tex = te ? atoi(te) : ts >= 1 && ts <= 2 ? 1 : 0;   // NR-22 implies NR-05 (=1) when DM_NR_TEX is unset
    const int mode = asked == 2 ? 2 : asked == 1 || (tex >= 1 && tex <= 3) ? 1 : 0;   // NR-05 needs the hooks: the registry at least
    if (mode != 1 && mode != 2) return;   // default: no hook, no tap, nothing allocated
    module_t *m = dn_main();
    if (!m) { LOG("【红】[nr] no main module: DM_NR off"); return; }
    g_reg = nr_reg_new();
    if (!g_reg) { LOG("【红】[nr] no memory for the registry: DM_NR off"); return; }
    g_nr_main = m; g_nr_win.t0 = nr_now();
    { const char *d = getenv("DM_NR_DUMP"); g_nr_dump = d && atoi(d) == 1; }
    { const char *b = getenv("DM_NR_BUF"); const int v = b ? atoi(b) : 0; g_nr_buf = v >= 1 && v <= 3 && xbg_nr_buf_event && xbg_nr_ring_set ? v : 0; }   // NR-06
    g_dm_nr = mode;   // before the first hook goes in (they read it); guest code is not running yet
    int in = 0;
    for (int i = 0; i < NR_SITES; i++) {
        const struct nr_site *s = &k_nr_site[i];
        void *o = NULL;
        if (!dn_words_ok(m, s->off, s->pro)) { LOG("【红】[nr] main+%#x (%s) differs: not DOS2 update v786432 main (build 4979b200): this hook stays off", s->off, s->name); continue; }
        if (dn_hook(m, s->off, s->pro, s->hook, &o)) { LOG("【红】[nr] main+%#x (%s): mmap / mprotect failed: this hook stays off", s->off, s->name); continue; }
        *s->orig = (nr_fn)o;
        in++;
    }
    const int key = nk_mode();   // NR-02: DM_NR_KEY 0 / 1 / 2 (read here, armed below)
    const int ev = nrnev_arm(m);   // [DOS2] NR-NATIVE step E: DM_NRN_EV (its table and hooks before any other hook runs)
    const int rc = dn_install_rc(key ? &k_nk_rc : ev ? &k_nr_rc_ev : &k_nr_rc);   // checks 0x93e5f0 itself; with DM_NR_KEY: the same opcode-55 handler + list depth
    if (rc) LOG("【红】[nr] no ExecuteRenderCommand handler (see above): resource transitions (opcode 55) not recorded");
    if (!in && rc) { g_dm_nr = 0; LOG("【红】[nr] nothing hooked: DM_NR off"); return; }
    if (tex >= 1 && tex <= 3 && in && nrt_start(tex, ts)) LOG("【红】[nr] DM_NR_TEX=%d: no memory / thread for the texture table: DM_NR_TEX off", tex);
    { const char *p = getenv("DM_NR_PIPE"); const int pm = p ? atoi(p) : 0;   // NR-07: before any guest code (and before the warm-up starts); the tap runs (g_dm_nr != 0)
      if (pm >= 1 && pm <= 3 && xbg_nr_pipe_hint && xbg_nr_pipe_source && xbg_shader_program_key) { g_nr_pipe = pm; xbg_nr_pipe_source(1); }
      else if (pm) LOG("【红】[nr] DM_NR_PIPE=%s: %s - no hints (the warm-up keeps list order)", p, pm < 1 || pm > 3 ? "not 1..3" : "no backend entry point / pack key function"); }
    LOG("[nr] DM_NR=%d (%s): engine object registry, %d of %d hooks in, opcode 55 %s, shader pack keys %s, DM_NR_DUMP=%d, DM_NR_BUF=%d (NR-06 events %s); the originals run with unchanged arguments, nothing is drawn differently",
        mode, mode == 2 ? "registry + census + checks" : "registry + census", in, NR_SITES, rc ? "off" : "in the shared ExecuteRenderCommand chain",
        xbg_shader_program_key ? "on (control + code, as the shader pack)" : "off (no xbg_shader_program_key)", g_nr_dump, g_nr_buf, g_nr_buf ? "to the engine" : "off");
    nrd_arm(m);   // NR-09R: DM_NR_DIRECT (before NR-23: =2 arms its compare)
    nc_arm(m);   // NR-23: DM_NR_NOCALL (before the flush hook goes in: it picks the hook function)
    nrs_arm(m);   // NR-09S: DM_NR_STATE (code hashes, jump table)
    nru_arm(m);   // NR-09U: DM_NR_UBO (code hashes)
    if (key) nk_install(m, key, rc == 0);
    nk_draw_install(m, rc == 0);   // NR-09: DM_NR_DRAW (after DM_NR_KEY: the identity comes from its hook, or from the hook alone)
    nrn_arm(g_nr_draw != 0);   // NR-NATIVE: DM_NRN_<PASS> (after DM_NR_DRAW: the card follows its tail)
    nrnd_arm(m, rc == 0);   // [DOS2] NR-NATIVE step D: DM_NRN_LIST (after the passes are armed and the ExecuteRenderCommand handler is registered)
    nc_install();   // NR-23: after the flush hook (needs it)
    nrd_install();   // NR-09R: after DM_NR_DRAW and NR-23 (needs the identity; =1 takes the draw hook's native handler)
    nru_install(g_nrd, g_nrd_parts);   // NR-09U: inside NR-09R's native draw handler
    nrub_install(g_nrd, g_nrd_parts);   // NR-UBD: the uniform-buffer mirror of NR-09R's served direct draws
    nrnb_arm();   // [DOS2] NR-NATIVE step D half b: DM_NRN_NOBODY (after NR-09R / NR-09U / NR-UBD: it stands in for their bodies' calls)
    nrs_install();   // NR-09S: the native opcode-21 handler (after the ExecuteRenderCommand handlers of NB / NR are registered)
    if (g_nr_pipe) LOG("[nr] DM_NR_PIPE=%d (NR-07): the pack key of every program the game sets up (nvnProgramSetShaders, inside or outside an engine call) goes to the warm-up as a hint", g_nr_pipe);
}
#endif
