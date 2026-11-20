// [DOS2] VK-NATIVE stage 4, part "creation without NVN" (dm_vkn_create_register; DM_VK_NATIVE=1 only): the engine's creation-time
// NVN calls (builders, *Initialize, *Finalize, pool registration, WriteTexels, the Texture* / Builder* getters, buffers and memory
// pools, queue / window / sync / command-buffer set-up of device init) go to small host functions of this file instead of HLE NVN.
// main = main.raw of update v786432, build 4979b200; r = the renderer ([main+0x2bbdc68]); dev = [r+0xb70]; the game's NVN function
// table = 515 u64 slots at main+0x2bbee98 (slot i at +8i; index -> name: src/work/re-nr-nrend/nvn_slot_map.tsv), filled twice by
// 0x934d70(device, GetProcAddress) inside device init 0x938330 (0x93852c with a null device, 0x938560 after nvnDeviceInitialize).
//
// Approach: (b) for every function of the task - the engine bodies run as they are and only their NVN entries are answered here.
// Why not (a): every body is mostly engine bookkeeping that other engine code reads (handle tables and generations, the pool-id
// bitmap dev+0x5a38 with its cursor dev+0x25a3c, the sampler-id bitmap dev+0x5838 via 0x937eb0, the default-sampler ids at object
// +0x100, record fields +0x10..+0x1b, the allocation record A at object +0xe0 from GpuMemAlloc 0x932900, the deferred-destroy tables
// r+0xdc0 / r+0x1180, the shared RT storage hash r+0x18c8), and the engine also writes texture storage on the CPU after creation
// through A (0xd1ada0 video planes, the immediate branches of 0xff5500 / 0x14284c0: TextureGetStride + memcpy into [object +0xe0]):
// keeping the bodies keeps all of that bit-exact, and the NVN entries they call are pure object set-up once nothing translates.
// Verified in the decompilations (the call lists: src/work/re-nr-nrend/nvn_by_function.txt, every call is `ldr xN,[table+8i]; blr`):
//   0x91e2a0 CreateTexture / 0x91f2e0 CreateTextureFromImage (+ 0x920230 / 0x92ccb0 / 0x92c8f0 / 0x92cf90 / 0x92ed30 default textures /
//            0x920460 mip reduction, which only call them): TextureBuilder* (the storage size / alignment / stride the engine allocates
//            by come from nvn_tex_layout, the same function HLE NVN used: same numbers), TextureInitialize, TexturePoolRegister-
//            Texture / -Image, TextureView*, TextureGet* (loops over depth / levels), DeviceGetInteger(0x38 / 0x39: linear stride
//            alignment), TextureWriteTexels. (0x91f2e0's "QueueBuilderGetControlMemorySize" rows in nvn_caller_index.tsv are false
//            matches: 0x9201f0 / 0x920210 are `ldr x8,[x22,#0x28]` of a lock, no call.)
//   0x93c3a0 window textures, 0x914bb0 shared RT storage (builder + GetStorageSize / Alignment only), 0x91d630 destroy
//            (TextureFinalize; the pending table r+0x1180 is drained through it), 0x921e40 / 0x9239f0 samplers, 0x922530 / 0x922d10 /
//            0x923360 states and 0x923c20 / 0x923e60 / 0x924080 their destroys (state setters on the record's NVN words; in mode 1 no
//            code reads those words: the exec part's ApplyBlend replacement documents it), 0x917100 -> 0x9181d0 vertex formats
//            (attrib / stream setters), 0x915ad0 CreateShader (no NVN call itself) -> 0x919090 CreateStageProgram: the code goes into
//            a class-2 pool block by the engine's own memcpy, program +0xe8 = MemoryPoolGetBufferAddress + offset (a host pointer, as
//            in HLE NVN: the shader part reads it), then ProgramInitialize / SetShaders; the reflection arrays of the record (+0x20..
//            +0x3a) and the hash tables come from the blob's type-3 section (0x915ad0, program +0xf0 / +0x180), not from NVN;
//            0x91ce90 shader destroy: ProgramFinalize x3.
//   0x932900 GpuMemAlloc -> 0x934150 pool create / 0x934910 pool destroy (MemoryPool*): the engine allocates the storage itself
//            (0x7cec50 / 0x1bf4e00) and hands it to MemoryPoolBuilderSetStorage, Map / GetBufferAddress return it (HLE NVN did the same).
//   UI block (not this piece's, but the same slots): 0x1bf2c60 / 0x1bf2860 GDraw texture / target (post-hooked by the UI part, the
//            originals run), 0x1bea840 GDraw init (states, samplers), 0x1beba70 programs, 0x1bf1700 / 0x1bf1ba0 buffers, 0x1bec360 /
//            0x1bec540 finalisation, 0x1bf4140 / 0x1bf1a10 / 0x1befd00 Texture getters, and the UI part's own host calls of slots
//            209 / 210 (TextureGetWidth / Height through the table): answered here with the same values.
//   Engine run-time getters (not creation, same slots): TextureGetWidth / Height / Depth (0xaf3b20 0xb02bc0 0xb1d6a0 0xb852a0
//            0xd1fd70 0xd20230 0xd25900 0x920b00), TextureGetFormat (0x9282c0), TextureGetStride (0xff5500 0x14284c0 0xd1ada0 0x93e5f0
//            0x9410f0), TextureView* (0x14227a0 0x93e5f0): answered from the object.
// Objects: every stub keeps the SAME in-place layout HLE NVN used (dm/gfx/nvn/nvn_obj.h; buf_t / buf_builder_t copied from hle_nvn.c,
// DOOM branch) without its side effects (no translator / Metal mirror, no snapshots or serials, no dirty-tracking ranges, no
// zero-copy pool mapping, no DM_NR taps, no write-tracking table): an object made here reads the same through any getter, and the
// command part's view_of (HLE view_t) and the UI part's TextureGetWidth calls keep working. TextureWriteTexels writes nothing: the
// creation data reaches the native image through the texture hooks from the engine arguments (hle_dos2_vkn_tex.c: initData, the
// image's level pointers, packaged storage); nothing else calls WriteTexels except the UI's 0x1bedc80 (replaced by the UI part).
// So the pool pages of sampled textures are never written by the host any more (HLE copied every texel into them).
//
// When: the stubs go into the table in a pre-hook of 0x932260 (the memory-pool set-up, called once at 0x9385e8 from device init,
// after the second table fill and before the first creation call: pools, texture / sampler pools, queue, syncs, contexts 0x939850,
// the renderer 0x939b00 -> 0x913170 (16 shared RT reservations 0x914bb0), 26 samplers, 10 blend / 18 depth-stencil / 10 raster
// states, 0x92b360). The command part's repointing after device init may call dm_vkn_create_slot again (re-asserts; harmless).
// DM_VK_NATIVE_CREATE (read at install): unset / 1 = all groups; 0 = none (the old behaviour: HLE NVN answers every creation entry);
// letters = only those groups, for a phone bisect: s states + vertex formats, m samplers, h shaders (programs), t textures (builders,
// textures, views, pool registration, texture handles), b buffers + memory pools, d device level (GetInteger, fast clears, timestamp,
// queue, window, texture / sampler pools, syncs, command-buffer set-up). Census every 10 s: calls by group, WriteTexels taken, and
// every unexpected condition (red, the first 16 of each with the values).
//
// ---- NVN entries the game can still reach in mode 1 after this piece (DM_VK_NATIVE_CREATE=1, the command part wired) ----
//   slot   name                         callers                      why it remains
//   0      DeviceBuilderSetDefaults     0x938330 (once)              device bootstrap: called through the table right after the first
//   3      DeviceInitialize             0x938330 (once)                fill (0x93852c), before any point a hook can repoint it (0x934d70
//   16     DeviceSetDepthMode           0x938330 (once)                cannot be hooked: x8 is live into its GetProcAddress call); the HLE
//   15     DeviceSetWindowOriginMode    0x938330 (once)                device object dev+8 is only stored, nothing reads it in mode 1
//   -      DeviceGetProcAddress         0x934d70 (2 x 515 names)     the table fill itself (bootstrap loader import 0x1bf6710 at main+0x2bbfeb8)
//   497    QueueFenceSync               0x939850 (0x9399dc, 3 times) the command part's slot, FORWARD for creation callers: writes the
//                                                                      sync's fence word (HLE x_fence), nothing else; start-up only
//   77     WindowAcquireTexture         0x9215f0 only                command part, FORWARD; the caller is replaced in mode 1: unreachable
//   (Not NVN: nv:: graphics init imports 0x1bf66b0..0x1bf6700 in 0x938330 = OS HLE, as every other nn:: service.)
//   Distance from empty: 4 slot calls + the table fill + 3 forwarded fences, all inside device init before / at its first creation
//   call; nothing after device init. With DM_VK_NATIVE_CREATE=0 every row of this file's X list below goes back to HLE NVN.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/nvn/nvn_obj.h"      // the in-place layouts HLE NVN uses (tex_t, view_t, samp_t, pool_t, ... and nvn_tex.h)
#include "../gfx/nvn/nvn_protos.h"   // NVNT_* prototypes: every stub is checked against its NVN signature
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"

#ifndef DM_GAME_DOOM
#error "the in-place layouts below are nvn_obj.h's DOOM branch, the one the DOS2 build (DOOM core) uses"
#endif
#define CR_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

// What the command part's repointing loop calls (weak on its side): 1 + *fn = the host function this file puts into slot
// `slot_index` of the game's NVN table (mode 1, its group on); 0 = not this file's slot (leave it).
int dm_vkn_create_slot(unsigned slot_index, void **fn);
void dm_vkn_create_register(void);

enum { M_TABLE = 0x2bbee98, TABLE_SLOTS = 515 };
enum { G_STA, G_SMP, G_SHD, G_TEX, G_BUF, G_DEV, G_N };
static const char k_gletter[G_N] = {'s', 'm', 'h', 't', 'b', 'd'};
static const char *const k_gname[G_N] = {"state", "sampler", "shader", "texture", "buffer/pool", "device"};

// ---- statistics ----
enum {
    R_DEVINFO, R_FMT, R_BUF_POOL, R_POOL_MEM, R_READ, R_PROG_N, R_WIN_N, R_VA_OFF, R_TABLE_NULL, R_HOOK_LATE, R_TEX_NOMEM, R_N
};
static const char *const k_rwhy[R_N] = {
    [R_DEVINFO] = "DeviceGetInteger: an item beyond the Tegra X1 table: 0 returned",
    [R_FMT] = "TextureBuilderGetStorageSize: a format nvn_tex does not know: size 0",
    [R_BUF_POOL] = "BufferInitialize: storage outside its memory pool: FALSE returned (as HLE NVN)",
    [R_POOL_MEM] = "MemoryPoolInitialize: a non-virtual pool without storage",
    [R_READ] = "TextureReadTexels reached (its callers 0x9291c0 / 0x9296d0 / 0x929880 are replaced in mode 1): destination zero-filled",
    [R_PROG_N] = "ProgramSetShaders with a stage count outside 0..6: FALSE returned (as HLE NVN)",
    [R_WIN_N] = "WindowBuilderSetTextures with a count outside 0..4: clamped",
    [R_VA_OFF] = "VertexAttribStateSetFormat: offset beyond the packed 19 bits",
    [R_TABLE_NULL] = "the game's NVN table holds a null entry for a creation slot (repointed anyway)",
    [R_HOOK_LATE] = "the stubs went in only after device init (the 0x932260 pre-hook did not run): device-init objects were made by HLE NVN",
    [R_TEX_NOMEM] = "TextureInitialize without pool storage (texture memory pointer NULL)",
};
static uint64_t g_n[TABLE_SLOTS], g_prev_grp[G_N], g_rc[R_N], g_wt_bytes, g_wt_calls;
static uint32_t g_rred[R_N];
static void red(int k, uint64_t a, uint64_t b, uint64_t c) {
    __atomic_add_fetch(&g_rc[k], 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&g_rred[k], 1, __ATOMIC_RELAXED) <= 16)
        vkn_logf(CR_RED "create: %s (values %#llx %#llx %#llx)", k_rwhy[k], (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
}
static inline uint64_t ld(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

// ---- the stub definition: C(slot, NVN name without "nvn", return type, parameters...) checks the prototype against NVNT_nvn<name>;
// N(name) at the top of the body counts the call ----
#define C(i, name, ret, ...)                                                                        \
    enum { I_##name = (i) };                                                                        \
    static ret c_##name(__VA_ARGS__);                                                               \
    static const NVNT_nvn##name k_chk_##name __attribute__((unused)) = c_##name;                    \
    static ret c_##name(__VA_ARGS__)
#define N(name) __atomic_add_fetch(&g_n[I_##name], 1, __ATOMIC_RELAXED)
#define B8(x) ((x) & 0xff)   // NVNboolean arguments: the low byte, as HLE NVN reads them
#define MAGIC4(a, b, c, d) MAGIC(a, b, c, d)

// ======== device level (group d) ========
// Device properties: HLE NVN's table (hle_nvn.c g_devinfo: Tegra X1 values, nn headers' nvn_DeviceConstantsNX.h), copied as it is.
static const int k_devinfo[] = {
    [0x00] = 53, [0x01] = 311,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};
C(7, DeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    N(DeviceGetInteger); (void)d;
    const unsigned w = (unsigned)what;
    if (!out) return;
    if (w >= sizeof k_devinfo / sizeof *k_devinfo) { red(R_DEVINFO, w, 0, 0); *out = 0; return; }
    *out = k_devinfo[w];
}
C(17, DeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *c, NVNformat f) { N(DeviceRegisterFastClearColor); (void)d; (void)c; (void)f; return 1; }
C(20, DeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d, float z) { N(DeviceRegisterFastClearDepth); (void)d; (void)z; return 1; }
// Counter timestamps are NVN's 614.4 MHz ticks (384 per 625 ns), as HLE NVN converts them. Only the replaced BeginFrame 0x9215f0 asks.
C(23, DeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *d, const NVNcounterData *c) {
    N(DeviceGetTimestampInNanoseconds); (void)d;
    return c ? (uint64_t)((__uint128_t)c->timestamp * 625 / 384) : 0;
}
// Queue (0x938330): the engine sizes and allocates the queue memory itself from GetQueueMemorySize (dev+0x51e8).
#define QB(b) ((queue_builder_t *)(b))
C(35, QueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { N(QueueBuilderSetDevice); QB(b)->dev = d; }
C(36, QueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    N(QueueBuilderSetDefaults);
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // NVN's device defaults (as HLE NVN)
}
C(38, QueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { N(QueueBuilderSetCommandMemorySize); QB(b)->cmd_sz = n; }
C(39, QueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { N(QueueBuilderSetComputeMemorySize); QB(b)->comp_sz = n; }
C(40, QueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { N(QueueBuilderSetControlMemorySize); QB(b)->ctl_sz = n; }
C(41, QueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    N(QueueBuilderGetQueueMemorySize);
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;   // HLE NVN's formula: the game allocates this much
}
C(42, QueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { N(QueueBuilderSetQueueMemory); QB(b)->mem = m; QB(b)->mem_sz = n; }
C(43, QueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { N(QueueBuilderSetCommandFlushThreshold); QB(b)->flush = n; }
C(54, QueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    N(QueueInitialize);
    *(queue_t *)q = (queue_t){MAGIC4('Q', 'U', 'E', 'U'), ((const queue_builder_t *)b)->dev, 0};
    return 1;
}
// Window (0x939120 builds it over the two window textures of 0x93c3a0; 0x93cc20 sets the crop): stored, nothing presents through
// it in mode 1 (the frame part presents its own images).
#define WB(b) ((win_builder_t *)(b))
C(62, WindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { N(WindowBuilderSetDevice); WB(b)->dev = d; }
C(63, WindowBuilderSetDefaults, void, NVNwindowBuilder *b) {
    N(WindowBuilderSetDefaults);
    NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1;
}
C(64, WindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { N(WindowBuilderSetNativeWindow); WB(b)->native = w; }
C(65, WindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    N(WindowBuilderSetTextures);
    if (n < 0 || n > 4) { red(R_WIN_N, (uint64_t)(int64_t)n, 0, 0); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n;
    for (int i = 0; i < n; i++) WB(b)->tex[i] = t ? t[i] : NULL;
}
C(66, WindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { N(WindowBuilderSetPresentInterval); WB(b)->interval = v; }
C(74, WindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    N(WindowInitialize);
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); o->magic = MAGIC4('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    vkn_logf("create: window %p initialized here (%d textures, interval %d): no HLE NVN window", (void *)w, o->ntex, o->interval);
    return 1;
}
C(81, WindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    N(WindowSetCrop);
    win_t *o = (win_t *)w; o->crop[0] = x; o->crop[1] = y; o->crop[2] = ww; o->crop[3] = hh;
}
// Descriptor pools (dev+0x57f8 / dev+0x5818, 0x938330): {memory pool, offset, count}; the ids are the engine's own bitmaps.
C(110, TexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    N(TexturePoolInitialize); *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n}; return 1;
}
C(118, SamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    N(SamplerPoolInitialize); *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n}; return 1;
}
// Command buffers of the three contexts (0x939850: Initialize, SetMemoryCallback, AddCommandMemory, AddControlMemory). In mode 1 no
// command is ever recorded through HLE NVN (the command part owns every recording slot and the replaced BeginFrame never adds
// memory again): the object only carries what HLE NVN's layout would (the memory segment, the callback).
C(352, CommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    N(CommandBufferInitialize);
    memset(c, 0, sizeof(cmd_t)); ((cmd_t *)c)->magic = MAGIC4('C', 'M', 'D', 'B'); ((cmd_t *)c)->dev = d;
    return 1;
}
C(355, CommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { N(CommandBufferSetMemoryCallback); ((cmd_t *)c)->cb = cb; }
C(358, CommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    N(CommandBufferAddCommandMemory);
    cmd_t *o = (cmd_t *)c; const pool_t *pl = (const pool_t *)p;
    if (pl && pl->mem) { o->seg = o->cur = pl->mem + off; o->end = o->cur + n; }
}
C(359, CommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *m, size_t n) { N(CommandBufferAddControlMemory); (void)c; (void)m; (void)n; }
C(494, SyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { N(SyncInitialize); (void)d; *(sync_t *)s = (sync_t){MAGIC4('S', 'Y', 'N', 'C'), 0, 0}; return 1; }

// ======== memory pools and buffers (group b) ========
// The engine's pools (0x934150): storage allocated by the engine, CPU pointer = GPU address = that storage (HLE NVN's convention,
// which the shader part relies on: program +0xe8). Buffers: the ring / per-frame buffers of 0x938330, the command parts' context
// buffers of 0x939850, the staged copy's temporary buffers (0x9218c0, kinds >= 2), GDraw's buffers 0x1bf1700 / 0x1bf1ba0.
#define PB(b) ((pool_builder_t *)(b))
C(92, MemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { N(MemoryPoolBuilderSetDevice); PB(b)->dev = d; }
C(93, MemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) { N(MemoryPoolBuilderSetDefaults); memset(b, 0, sizeof(pool_builder_t)); }
C(94, MemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { N(MemoryPoolBuilderSetStorage); PB(b)->mem = m; PB(b)->size = n; }
C(95, MemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { N(MemoryPoolBuilderSetFlags); PB(b)->flags = (uint32_t)f; }
C(100, MemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    N(MemoryPoolInitialize);
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    o->magic = MAGIC4('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = (uint8_t *)s->mem; o->size = s->size;
    if (!o->mem && !(o->flags & 0x200)) red(R_POOL_MEM, o->flags, o->size, (uint64_t)(uintptr_t)p);   // 0x200 = virtual
    return 1;
}
C(102, MemoryPoolFinalize, void, NVNmemoryPool *p) { N(MemoryPoolFinalize); ((pool_t *)p)->magic = 0; }
C(103, MemoryPoolMap, void *, const NVNmemoryPool *p) { N(MemoryPoolMap); return ((const pool_t *)p)->mem; }
C(106, MemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { N(MemoryPoolGetBufferAddress); return (NVNbufferAddress)(uintptr_t)((const pool_t *)p)->mem; }
// hle_nvn.c's buffer layout (its DOOM branch, the one DOS2 builds: DM_GAME_DOOM without DM_GAME_ETERNAL), copied: not in a header.
typedef struct { NVNdevice *dev; pool_t *pool; int64_t off; uint64_t size; } cr_buf_builder_t;     FITS(cr_buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; pool_t *pool; int64_t off; uint64_t size; uint8_t *mem; } cr_buf_t;  FITS(cr_buf_t, NVNbuffer);
#define BB(b) ((cr_buf_builder_t *)(b))
C(126, BufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { N(BufferBuilderSetDevice); BB(b)->dev = d; }
C(127, BufferBuilderSetDefaults, void, NVNbufferBuilder *b) {
    N(BufferBuilderSetDefaults); NVNdevice *d = BB(b)->dev; memset(b, 0, sizeof(cr_buf_builder_t)); BB(b)->dev = d;
}
C(128, BufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    N(BufferBuilderSetStorage); BB(b)->pool = (pool_t *)p; BB(b)->off = off; BB(b)->size = n;
}
C(133, BufferInitialize, NVNboolean, NVNbuffer *buf, const NVNbufferBuilder *b) {
    N(BufferInitialize);
    const cr_buf_builder_t *s = (const cr_buf_builder_t *)b; cr_buf_t *o = (cr_buf_t *)buf;
    if (!s->pool || !s->pool->mem || s->off < 0 || (uint64_t)s->off + s->size > s->pool->size) {
        red(R_BUF_POOL, (uint64_t)(uintptr_t)s->pool, (uint64_t)s->off, s->size);
        return 0;
    }
    o->magic = MAGIC4('B', 'U', 'F', 'F'); o->pool = s->pool; o->off = s->off; o->size = s->size; o->mem = s->pool->mem + s->off;
    return 1;
}
C(135, BufferFinalize, void, NVNbuffer *b) { N(BufferFinalize); ((cr_buf_t *)b)->magic = 0; }
C(136, BufferMap, void *, const NVNbuffer *b) { N(BufferMap); return ((const cr_buf_t *)b)->mem; }
C(137, BufferGetAddress, NVNbufferAddress, const NVNbuffer *b) { N(BufferGetAddress); return (NVNbufferAddress)(uintptr_t)((const cr_buf_t *)b)->mem; }
C(142, BufferGetSize, size_t, const NVNbuffer *b) { N(BufferGetSize); return (size_t)((const cr_buf_t *)b)->size; }

// ======== textures (group t) ========
#define TB(b) ((tex_builder_t *)(b))
#define CTB(b) ((const tex_builder_t *)(b))
#define CTX(t) ((const tex_t *)(t))
#define SWZ_IDENTITY (2u | 3u << 3 | 4u << 6 | 5u << 9)   // HLE NVN's packed identity swizzle (r | g << 3 | b << 6 | a << 9)
C(10, DeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tex, int samp) {
    N(DeviceGetTextureHandle); (void)d; return (NVNtextureHandle)((uint64_t)(uint32_t)tex | (uint64_t)(uint32_t)samp << 20);
}
C(12, DeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { N(DeviceGetImageHandle); (void)d; return (NVNimageHandle)id; }
// Pool registration: the engine keeps the ids (object +0xc0.. and the bitmap); the native images are found by those ids (vkn_tex).
C(113, TexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) { N(TexturePoolRegisterTexture); (void)tp; (void)id; (void)t; (void)v; }
C(114, TexturePoolRegisterImage, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) { N(TexturePoolRegisterImage); (void)tp; (void)id; (void)t; (void)v; }
C(144, TextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { N(TextureBuilderSetDevice); TB(b)->dev = d; }
C(145, TextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    N(TextureBuilderSetDefaults);
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
C(146, TextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { N(TextureBuilderSetFlags); TB(b)->d.flags = f; }
C(147, TextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { N(TextureBuilderSetTarget); TB(b)->d.target = (int)t; }
C(152, TextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) {
    N(TextureBuilderSetSize2D); TB(b)->d.width = (uint32_t)w; TB(b)->d.height = (uint32_t)h; TB(b)->d.depth = 1;
}
C(153, TextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int dp) {
    N(TextureBuilderSetSize3D); TB(b)->d.width = (uint32_t)w; TB(b)->d.height = (uint32_t)h; TB(b)->d.depth = (uint32_t)dp;
}
C(154, TextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { N(TextureBuilderSetLevels); TB(b)->d.levels = (uint32_t)v; }
C(155, TextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { N(TextureBuilderSetFormat); TB(b)->d.format = (int)f; }
C(156, TextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { N(TextureBuilderSetSamples); TB(b)->d.samples = s; }
C(157, TextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    N(TextureBuilderSetSwizzle);
    tex_builder_t *o = TB(b); o->swz = (uint16_t)((r & 7) | (g & 7) << 3 | (bl & 7) << 6 | (a & 7) << 9); o->set_swz = o->swz != SWZ_IDENTITY;
}
C(158, TextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) {
    N(TextureBuilderSetDepthStencilMode); TB(b)->stencil = m == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
}
// Storage size / alignment: nvn_tex_layout (dm/gfx/nvn/nvn_tex.c), the function HLE NVN answered with: the engine allocates its
// pool blocks (GpuMemAlloc 0x932900) and the shared RT reservations (0x914bb0) by exactly these numbers, as before.
C(159, TextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    N(TextureBuilderGetStorageSize);
    nvn_texlayout L; nvn_tex_layout(&CTB(b)->d, &L);
    if (!L.bpb) red(R_FMT, (uint64_t)(uint32_t)CTB(b)->d.format, CTB(b)->d.width, CTB(b)->d.height);
    return (size_t)L.size;
}
C(160, TextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    N(TextureBuilderGetStorageAlignment); nvn_texlayout L; nvn_tex_layout(&CTB(b)->d, &L); return L.align;
}
C(161, TextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { N(TextureBuilderSetStorage); TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
C(163, TextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    N(TextureBuilderSetPackagedTextureLayout);
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
C(164, TextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { N(TextureBuilderSetStride); TB(b)->d.stride = (uint32_t)s; }
C(170, TextureBuilderGetWidth, int, const NVNtextureBuilder *b) { N(TextureBuilderGetWidth); return (int)CTB(b)->d.width; }
C(171, TextureBuilderGetHeight, int, const NVNtextureBuilder *b) { N(TextureBuilderGetHeight); return (int)CTB(b)->d.height; }
C(172, TextureBuilderGetDepth, int, const NVNtextureBuilder *b) { N(TextureBuilderGetDepth); return (int)CTB(b)->d.depth; }
C(180, TextureBuilderGetStride, ptrdiff_t, const NVNtextureBuilder *b) { N(TextureBuilderGetStride); return (ptrdiff_t)CTB(b)->d.stride; }
// Views: HLE NVN's view_t in place (the command part's view_of reads it for ClearTexture / SetRenderTargets).
C(187, TextureViewSetDefaults, void, NVNtextureView *v) { N(TextureViewSetDefaults); memset(v, 0, sizeof(view_t)); }
C(188, TextureViewSetLevels, void, NVNtextureView *v, int base, int n) {
    N(TextureViewSetLevels); view_t *w = (view_t *)v; w->set_levels = 1; w->base_level = base; w->nlevels = n;
}
C(189, TextureViewSetLayers, void, NVNtextureView *v, int min, int n) {
    N(TextureViewSetLayers); view_t *w = (view_t *)v; w->set_layers = 1; w->min_layer = min; w->nlayers = n;
}
C(193, TextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) {
    N(TextureViewSetTarget); view_t *w = (view_t *)v; w->set_target = 1; w->target = (int32_t)t;
}
// The texture object (engine object +0, 0xc0 bytes): HLE NVN's tex_t, so every getter (the engine's run-time ones included) answers
// what the builder said. mem = the pool storage + offset: the engine's CPU writers use [object +0xe0] (A), not this.
C(201, TextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    N(TextureInitialize);
    const tex_builder_t *s = CTB(b); tex_t *o = (tex_t *)t;
    memset(o, 0, sizeof *o);
    o->magic = MAGIC4('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8);
    o->set_swz = s->set_swz; o->stencil = s->stencil; o->swz = s->swz;
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    if (!o->mem) __atomic_add_fetch(&g_rc[R_TEX_NOMEM], 1, __ATOMIC_RELAXED);   // (counted only: HLE NVN accepted it too)
    return 1;
}
C(203, TextureFinalize, void, NVNtexture *t) { N(TextureFinalize); ((tex_t *)t)->magic = 0; }
C(207, TextureGetFlags, NVNtextureFlags, const NVNtexture *t) { N(TextureGetFlags); return (NVNtextureFlags)CTX(t)->d.flags; }
C(209, TextureGetWidth, int, const NVNtexture *t) { N(TextureGetWidth); return (int)CTX(t)->d.width; }
C(210, TextureGetHeight, int, const NVNtexture *t) { N(TextureGetHeight); return (int)CTX(t)->d.height; }
C(211, TextureGetDepth, int, const NVNtexture *t) { N(TextureGetDepth); return (int)CTX(t)->d.depth; }
C(212, TextureGetLevels, int, const NVNtexture *t) { N(TextureGetLevels); return (int)CTX(t)->d.levels; }
C(213, TextureGetFormat, NVNformat, const NVNtexture *t) { N(TextureGetFormat); return (NVNformat)CTX(t)->d.format; }
C(217, TextureGetStride, ptrdiff_t, const NVNtexture *t) { N(TextureGetStride); return (ptrdiff_t)CTX(t)->d.stride; }
static uint64_t region_bytes(const tex_t *o, const NVNcopyRegion *r) {
    int bw = 1, bh = 1; const int bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    if (!r || bpb <= 0 || r->width <= 0 || r->height <= 0) return 0;
    return (uint64_t)((r->width + bw - 1) / bw) * (uint64_t)((r->height + bh - 1) / bh) * (uint64_t)bpb * (uint64_t)(r->depth > 0 ? r->depth : 1);
}
// Creation data (0x91e2a0 / 0x91f2e0): the native image got it from the engine arguments in the texture hooks; counted only.
C(220, TextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) {
    N(TextureWriteTexels); (void)v; (void)src;
    __atomic_add_fetch(&g_wt_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_wt_bytes, region_bytes(CTX(t), r), __ATOMIC_RELAXED);
}
C(222, TextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    N(TextureReadTexels); (void)v;
    const uint64_t n = region_bytes(CTX(t), r);
    red(R_READ, (uint64_t)(uintptr_t)t, (uint64_t)(uint32_t)CTX(t)->d.format, n);
    if (dst && n) memset(dst, 0, (size_t)n);
}

// ======== samplers (group m) ========
#define SB(b) (&((samp_builder_t *)(b))->p)
C(121, SamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { N(SamplerPoolRegisterSampler); (void)sp; (void)id; (void)s; }
C(232, SamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { N(SamplerBuilderSetDevice); ((samp_builder_t *)b)->dev = d; }
C(233, SamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    N(SamplerBuilderSetDefaults);
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
C(234, SamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { N(SamplerBuilderSetMinMagFilter); SB(b)->minf = (uint8_t)mi; SB(b)->magf = (uint8_t)ma; }
C(235, SamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) {
    N(SamplerBuilderSetWrapMode); SB(b)->ws = (uint8_t)s; SB(b)->wt = (uint8_t)t; SB(b)->wr = (uint8_t)r;
}
C(238, SamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { N(SamplerBuilderSetCompare); SB(b)->cmp_mode = (uint8_t)m; SB(b)->cmp_func = (uint8_t)f; }
C(239, SamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { N(SamplerBuilderSetBorderColor); if (c) memcpy(SB(b)->border, c, 16); }
C(242, SamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { N(SamplerBuilderSetMaxAnisotropy); SB(b)->aniso = v; }
C(257, SamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    N(SamplerInitialize); samp_t *o = (samp_t *)s; o->magic = MAGIC4('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;
}
C(258, SamplerFinalize, void, NVNsampler *s) { N(SamplerFinalize); ((samp_t *)s)->magic = 0; }

// ======== programs (group h) ========
// 0x919090 / 0x1beba70: the engine copied the code into its pool block and wrote program +0xc8 / +0xe8 before these calls; the
// shader and UI parts read the blob and +0xe8 (hle_dos2_vkn_shader.c, hle_dos2_vkn_ui.c h_prog). The control sections are NOT
// copied (HLE NVN interned them for its translator, which is not running: the shader part copies what it needs at creation).
C(86, ProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    N(ProgramInitialize); (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC4('P', 'R', 'O', 'G'); return 1;
}
C(87, ProgramFinalize, void, NVNprogram *p) { N(ProgramFinalize); ((prog_t *)p)->magic = 0; }
C(89, ProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    N(ProgramSetShaders);
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { red(R_PROG_N, (uint64_t)(int64_t)n, (uint64_t)(uintptr_t)p, 0); return 0; }
    o->nstage = (uint32_t)n;
    if (n && sd) memcpy(o->sd, sd, (size_t)n * sizeof *sd);
    return 1;
}

// ======== state objects and vertex formats (group s): HLE NVN's packed in-place words (nvn_obj.h) ========
#define ST(T, x) ((T *)(x))
C(272, BlendStateSetDefaults, void, NVNblendState *s) {
    N(BlendStateSetDefaults);
    *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
}
C(273, BlendStateSetBlendTarget, void, NVNblendState *s, int t) { N(BlendStateSetBlendTarget); ST(blend_st, s)->target = (uint64_t)t; }
C(274, BlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    N(BlendStateSetBlendFunc); blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
C(275, BlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) {
    N(BlendStateSetBlendEquation); ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea;
}
C(287, ColorStateSetDefaults, void, NVNcolorState *s) { N(ColorStateSetDefaults); *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
C(288, ColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    N(ColorStateSetBlendEnable);
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B8(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
C(290, ColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) { N(ColorStateSetAlphaTest); ST(color_st, s)->alpha_func = f; }
C(294, ChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { N(ChannelMaskStateSetDefaults); ST(chmask_st, s)->mask = 0xffffffffu; }
C(295, ChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    N(ChannelMaskStateSetChannelMask);
    if (i < 0 || i > 7) return;
    const uint32_t m = (B8(r) ? 1u : 0u) | (B8(g) ? 2u : 0u) | (B8(b) ? 4u : 0u) | (B8(a) ? 8u : 0u);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
C(320, PolygonStateSetDefaults, void, NVNpolygonState *s) { N(PolygonStateSetDefaults); *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
C(321, PolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { N(PolygonStateSetCullFace); ST(poly_st, s)->cull = f; }
C(322, PolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { N(PolygonStateSetFrontFace); ST(poly_st, s)->front = f; }
C(323, PolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { N(PolygonStateSetPolygonMode); ST(poly_st, s)->mode = m; }
C(324, PolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { N(PolygonStateSetPolygonOffsetEnables); ST(poly_st, s)->offset_en = (uint32_t)e; }
C(329, DepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    N(DepthStencilStateSetDefaults);
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
C(330, DepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { N(DepthStencilStateSetDepthTestEnable); ST(ds_st, s)->dt = B8(e) != 0; }
C(331, DepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { N(DepthStencilStateSetDepthWriteEnable); ST(ds_st, s)->dw = B8(e) != 0; }
C(332, DepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { N(DepthStencilStateSetDepthFunc); ST(ds_st, s)->df = f; }
C(333, DepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { N(DepthStencilStateSetStencilTestEnable); ST(ds_st, s)->st = B8(e) != 0; }
C(334, DepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    N(DepthStencilStateSetStencilFunc); ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
C(335, DepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    N(DepthStencilStateSetStencilOp);
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
C(342, VertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { N(VertexAttribStateSetDefaults); *ST(va_st, s) = (va_st){0, 0, 0}; }
C(343, VertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    N(VertexAttribStateSetFormat);
    if (off < 0 || off >= (1 << 19)) red(R_VA_OFF, (uint64_t)off, (uint64_t)(uint32_t)f, 0);
    ST(va_st, s)->fmt = (uint32_t)f; ST(va_st, s)->offset = (uint32_t)off;
}
C(344, VertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { N(VertexAttribStateSetStreamIndex); ST(va_st, s)->stream = (uint32_t)i; }
C(347, VertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { N(VertexStreamStateSetDefaults); *ST(vs_st, s) = (vs_st){0, 0}; }
C(348, VertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { N(VertexStreamStateSetStride); ST(vs_st, s)->stride = (uint32_t)st; }
C(349, VertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { N(VertexStreamStateSetDivisor); ST(vs_st, s)->divisor = (uint32_t)d; }
C(350, VertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { N(VertexStreamStateGetStride); return (ptrdiff_t)((const vs_st *)s)->stride; }

// ---- the slot list (by group) ----
#define CR_SLOTS(X) \
    X(G_DEV, DeviceGetInteger) X(G_DEV, DeviceRegisterFastClearColor) X(G_DEV, DeviceRegisterFastClearDepth) \
    X(G_DEV, DeviceGetTimestampInNanoseconds) X(G_DEV, QueueBuilderSetDevice) X(G_DEV, QueueBuilderSetDefaults) \
    X(G_DEV, QueueBuilderSetCommandMemorySize) X(G_DEV, QueueBuilderSetComputeMemorySize) X(G_DEV, QueueBuilderSetControlMemorySize) \
    X(G_DEV, QueueBuilderGetQueueMemorySize) X(G_DEV, QueueBuilderSetQueueMemory) X(G_DEV, QueueBuilderSetCommandFlushThreshold) \
    X(G_DEV, QueueInitialize) X(G_DEV, WindowBuilderSetDevice) X(G_DEV, WindowBuilderSetDefaults) X(G_DEV, WindowBuilderSetNativeWindow) \
    X(G_DEV, WindowBuilderSetTextures) X(G_DEV, WindowBuilderSetPresentInterval) X(G_DEV, WindowInitialize) X(G_DEV, WindowSetCrop) \
    X(G_DEV, TexturePoolInitialize) X(G_DEV, SamplerPoolInitialize) X(G_DEV, CommandBufferInitialize) \
    X(G_DEV, CommandBufferSetMemoryCallback) X(G_DEV, CommandBufferAddCommandMemory) X(G_DEV, CommandBufferAddControlMemory) \
    X(G_DEV, SyncInitialize) \
    X(G_BUF, MemoryPoolBuilderSetDevice) X(G_BUF, MemoryPoolBuilderSetDefaults) X(G_BUF, MemoryPoolBuilderSetStorage) \
    X(G_BUF, MemoryPoolBuilderSetFlags) X(G_BUF, MemoryPoolInitialize) X(G_BUF, MemoryPoolFinalize) X(G_BUF, MemoryPoolMap) \
    X(G_BUF, MemoryPoolGetBufferAddress) X(G_BUF, BufferBuilderSetDevice) X(G_BUF, BufferBuilderSetDefaults) \
    X(G_BUF, BufferBuilderSetStorage) X(G_BUF, BufferInitialize) X(G_BUF, BufferFinalize) X(G_BUF, BufferMap) \
    X(G_BUF, BufferGetAddress) X(G_BUF, BufferGetSize) \
    X(G_TEX, DeviceGetTextureHandle) X(G_TEX, DeviceGetImageHandle) X(G_TEX, TexturePoolRegisterTexture) \
    X(G_TEX, TexturePoolRegisterImage) X(G_TEX, TextureBuilderSetDevice) X(G_TEX, TextureBuilderSetDefaults) \
    X(G_TEX, TextureBuilderSetFlags) X(G_TEX, TextureBuilderSetTarget) X(G_TEX, TextureBuilderSetSize2D) \
    X(G_TEX, TextureBuilderSetSize3D) X(G_TEX, TextureBuilderSetLevels) X(G_TEX, TextureBuilderSetFormat) \
    X(G_TEX, TextureBuilderSetSamples) X(G_TEX, TextureBuilderSetSwizzle) X(G_TEX, TextureBuilderSetDepthStencilMode) \
    X(G_TEX, TextureBuilderGetStorageSize) X(G_TEX, TextureBuilderGetStorageAlignment) X(G_TEX, TextureBuilderSetStorage) \
    X(G_TEX, TextureBuilderSetPackagedTextureLayout) X(G_TEX, TextureBuilderSetStride) X(G_TEX, TextureBuilderGetWidth) \
    X(G_TEX, TextureBuilderGetHeight) X(G_TEX, TextureBuilderGetDepth) X(G_TEX, TextureBuilderGetStride) \
    X(G_TEX, TextureViewSetDefaults) X(G_TEX, TextureViewSetLevels) X(G_TEX, TextureViewSetLayers) X(G_TEX, TextureViewSetTarget) \
    X(G_TEX, TextureInitialize) X(G_TEX, TextureFinalize) X(G_TEX, TextureGetFlags) X(G_TEX, TextureGetWidth) \
    X(G_TEX, TextureGetHeight) X(G_TEX, TextureGetDepth) X(G_TEX, TextureGetLevels) X(G_TEX, TextureGetFormat) \
    X(G_TEX, TextureGetStride) X(G_TEX, TextureWriteTexels) X(G_TEX, TextureReadTexels) \
    X(G_SMP, SamplerPoolRegisterSampler) X(G_SMP, SamplerBuilderSetDevice) X(G_SMP, SamplerBuilderSetDefaults) \
    X(G_SMP, SamplerBuilderSetMinMagFilter) X(G_SMP, SamplerBuilderSetWrapMode) X(G_SMP, SamplerBuilderSetCompare) \
    X(G_SMP, SamplerBuilderSetBorderColor) X(G_SMP, SamplerBuilderSetMaxAnisotropy) X(G_SMP, SamplerInitialize) \
    X(G_SMP, SamplerFinalize) \
    X(G_SHD, ProgramInitialize) X(G_SHD, ProgramFinalize) X(G_SHD, ProgramSetShaders) \
    X(G_STA, BlendStateSetDefaults) X(G_STA, BlendStateSetBlendTarget) X(G_STA, BlendStateSetBlendFunc) \
    X(G_STA, BlendStateSetBlendEquation) X(G_STA, ColorStateSetDefaults) X(G_STA, ColorStateSetBlendEnable) \
    X(G_STA, ColorStateSetAlphaTest) X(G_STA, ChannelMaskStateSetDefaults) X(G_STA, ChannelMaskStateSetChannelMask) \
    X(G_STA, PolygonStateSetDefaults) X(G_STA, PolygonStateSetCullFace) X(G_STA, PolygonStateSetFrontFace) \
    X(G_STA, PolygonStateSetPolygonMode) X(G_STA, PolygonStateSetPolygonOffsetEnables) X(G_STA, DepthStencilStateSetDefaults) \
    X(G_STA, DepthStencilStateSetDepthTestEnable) X(G_STA, DepthStencilStateSetDepthWriteEnable) \
    X(G_STA, DepthStencilStateSetDepthFunc) X(G_STA, DepthStencilStateSetStencilTestEnable) X(G_STA, DepthStencilStateSetStencilFunc) \
    X(G_STA, DepthStencilStateSetStencilOp) X(G_STA, VertexAttribStateSetDefaults) X(G_STA, VertexAttribStateSetFormat) \
    X(G_STA, VertexAttribStateSetStreamIndex) X(G_STA, VertexStreamStateSetDefaults) X(G_STA, VertexStreamStateSetStride) \
    X(G_STA, VertexStreamStateSetDivisor) X(G_STA, VertexStreamStateGetStride)
typedef struct { uint16_t idx; uint8_t grp; const char *name; void *fn; } cr_slot;
#define X_ROW(g, name) {I_##name, g, #name, (void *)c_##name},
static const cr_slot k_slots[] = { CR_SLOTS(X_ROW) };
#undef X_ROW
enum { CR_NSLOTS = sizeof k_slots / sizeof k_slots[0] };

// ---- install ----
static int g_on;                     // mode 1 and DM_VK_NATIVE_CREATE != 0
static unsigned g_groups;            // bit g = group g on
static void *g_stub[TABLE_SLOTS];    // the host-stack stub of each slot's function (made at registration)
static uint8_t g_grp_of[TABLE_SLOTS];   // group + 1 of a slot of this file (0 = not ours)
static uint64_t g_orig[TABLE_SLOTS];    // what the table held before (HLE NVN's thunk), for the log only
static int g_installed, g_prehook;   // the stubs are in the table / the 0x932260 pre-hook ran
int dm_vkn_create_slot(unsigned slot_index, void **fn) {
    if (fn) *fn = NULL;
    if (!g_on || slot_index >= TABLE_SLOTS || !g_grp_of[slot_index] || !(g_groups >> (g_grp_of[slot_index] - 1) & 1)) return 0;
    if (!g_stub[slot_index]) return 0;
    if (fn) *fn = g_stub[slot_index];
    return 1;
}
static void install(const char *where) {
    uint64_t *tab = (uint64_t *)(void *)(dm_vkn_base() + M_TABLE);
    int n = 0, already = 0, want = 0;
    for (int i = 0; i < CR_NSLOTS; i++) {
        void *f = NULL;
        if (!dm_vkn_create_slot(k_slots[i].idx, &f)) continue;
        want++;
        const uint64_t cur = __atomic_load_n(&tab[k_slots[i].idx], __ATOMIC_ACQUIRE), mine = (uint64_t)(uintptr_t)f;
        if (cur == mine) { already++; continue; }
        if (!cur) red(R_TABLE_NULL, k_slots[i].idx, 0, 0);
        g_orig[k_slots[i].idx] = cur;
        __atomic_store_n(&tab[k_slots[i].idx], mine, __ATOMIC_RELEASE);
        n++;
    }
    if (n + already) g_installed = 1;
    if (!n) return;
    char gs[G_N + 1]; int k = 0; for (int g = 0; g < G_N; g++) if (g_groups >> g & 1) gs[k++] = k_gletter[g]; gs[k] = 0;
    vkn_logf("create: %d of %d creation-time NVN entries of the game's table main+%#x repointed to the native front end %s (groups '%s'%s); "
             "left to HLE NVN: the device bootstrap (DeviceBuilderSetDefaults, DeviceInitialize, DeviceSetDepthMode, DeviceSetWindowOriginMode, "
             "the two table fills) and the command part's slots", n + already, want, M_TABLE, where, gs, already ? ", some were in already" : "");
}
// 0x932260(device, pool sizes...): the memory-pool set-up of device init (one caller, 0x9385e8 in 0x938330, after the second table
// fill at 0x938560 and before the first creation call). Pre-hook: the stubs go in, then the original creates the pools through them.
static vkn_fn g_o_pools;
VKN_HOOK cr_h_pools(VKN_A) {
    if (g_on) { g_prehook = 1; install("at device init's pool set-up 0x932260"); }
    return g_o_pools(VKN_F);
}

// ---- census ----
static void cr_census(void) {
    if (!g_on) return;
    uint64_t grp[G_N] = {0}, tot = 0;
    for (int i = 0; i < CR_NSLOTS; i++) { const uint64_t v = ld(&g_n[k_slots[i].idx]); grp[k_slots[i].grp] += v; tot += v; }
    char gl[400]; int k = 0; gl[0] = 0;
    for (int g = 0; g < G_N; g++) {
        k += snprintf(gl + k, sizeof gl - (size_t)k, "%s%s %llu (+%llu)%s", g ? ", " : "", k_gname[g], (unsigned long long)grp[g],
                      (unsigned long long)(grp[g] - g_prev_grp[g]), g_groups >> g & 1 ? "" : " off");
        g_prev_grp[g] = grp[g];
    }
    vkn_logf("create: stub calls %llu (%s); WriteTexels %llu (%.1f MB not copied: the texture hooks took the data); installed %s; textures without storage %llu",
             (unsigned long long)tot, gl, (unsigned long long)ld(&g_wt_calls), ld(&g_wt_bytes) / 1048576.0,
             g_installed ? (g_prehook ? "at device init" : "after device init") : "NOT (HLE NVN answers every creation entry)", (unsigned long long)ld(&g_rc[R_TEX_NOMEM]));
    uint64_t bad = 0; for (int i = 0; i < R_N; i++) if (i != R_TEX_NOMEM) bad += ld(&g_rc[i]);
    if (!bad) return;
    static const char *const nm[R_N] = {[R_DEVINFO] = "devinfo", [R_FMT] = "format", [R_BUF_POOL] = "buffer-pool", [R_POOL_MEM] = "pool-storage",
        [R_READ] = "read-texels", [R_PROG_N] = "program-stages", [R_WIN_N] = "window-textures", [R_VA_OFF] = "attrib-offset", [R_TABLE_NULL] = "table-null",
        [R_HOOK_LATE] = "late-install", [R_TEX_NOMEM] = "tex-no-storage"};
    char line[400]; int j = 0; line[0] = 0;
    for (int i = 0; i < R_N && j < (int)sizeof line - 48; i++) if (i != R_TEX_NOMEM && ld(&g_rc[i])) j += snprintf(line + j, sizeof line - (size_t)j, " %s %llu", nm[i], (unsigned long long)ld(&g_rc[i]));
    vkn_logf(CR_RED "create: unexpected:%s", line);
}
// The first census (the first presented frame): the pre-hook never ran (its site is missing) - the device-init objects were made by
// HLE NVN (same layouts, so the stubs read them alike); the stubs go in now unless the command part's repointing put them in already.
static void cr_check(void) {
    static int done;
    if (done || !g_on) return;
    done = 1;
    if (g_prehook) return;
    red(R_HOOK_LATE, (uint64_t)g_installed, 0, 0);
    install("late (first census: the 0x932260 pre-hook never ran)");
}
static void cr_census_all(void) { cr_check(); cr_census(); }

// ---- registration (dm_vkn_tex_register calls it; install time, single-threaded) ----
void dm_vkn_create_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;   // mode 2 keeps HLE NVN for every object (its old path needs them)
    const char *e = getenv("DM_VK_NATIVE_CREATE");
    g_groups = (1u << G_N) - 1;
    if (e && *e) {
        if (!strcmp(e, "0")) g_groups = 0;
        else if (strcmp(e, "1")) {
            unsigned m = 0; int bad = 0;
            for (const char *p = e; *p; p++) {
                const char *q = memchr(k_gletter, *p, G_N);
                if (q) m |= 1u << (q - k_gletter); else bad = 1;
            }
            if (bad || !m) vkn_logf(CR_RED "create: DM_VK_NATIVE_CREATE=%s: not 0 / 1 / letters of 'smhtbd': all groups on", e);
            else g_groups = m;
        }
    }
    if (!g_groups) { vkn_logf("create: DM_VK_NATIVE_CREATE=0: HLE NVN answers every creation-time entry (the old behaviour)"); return; }
    int nostub = 0;
    for (int i = 0; i < CR_NSLOTS; i++) {
        g_grp_of[k_slots[i].idx] = (uint8_t)(k_slots[i].grp + 1);
        // NVN functions take no stack arguments, so the stub (made for the VKN_A signature) hands every register to the function's
        // own prototype unchanged; the function runs on the thread's host stack like an HLE NVN entry did.
        if (!(g_stub[k_slots[i].idx] = dm_vkn_stack_stub(k_slots[i].fn))) nostub++;
    }
    if (nostub) vkn_logf(CR_RED "create: %d stub functions without a host-stack stub: those slots stay HLE NVN", nostub);
    g_on = 1;
    // First 4 instructions of 0x932260 (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone; one caller, bl).
    static const uint32_t k_pools[4] = {0xd10243ff /* sub sp,sp,#0x90 */, 0xa9036ffc, 0xa90467fa, 0xa9055ff8};
    dm_vkn_site(0x932260, k_pools, "DeviceInit pool set-up", (void *)cr_h_pools, &g_o_pools, VKN_M_NATIVE);
    dm_vkn_census(cr_census_all);
}
#endif
