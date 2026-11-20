// [DOS2] NR-23 (DM_NR_NOCALL, docs/dos2/nr/NR-23.md): interface between the NVN layer (nvn_nr_nocall.inc, inside hle_nvn.c) and the
// front end (dm/host/hle_dos2_nr_nocall.inc). Everything here is per thread or read-only after the install.
#ifndef DM_NVN_NR_NOCALL_H
#define DM_NVN_NR_NOCALL_H
#include <stdint.h>
// mode 0: nothing. 1: the direct entries may be used (after nvn_nc_ready). 2: + NVN calls counted per thread (through the big-stack
// wrapper) and the capture below.
void nvn_nc_enable(int mode);
// 1 = each of the five entry points the draw code of the engine calls (GetTextureHandle, BindTexture, DrawElements,
// DrawElementsInstanced, SetRenderEnable) is implemented by the function the direct entries below call (known wrappers - big stack,
// timing, trace - peeled); thunk[i] is the address GetProcAddress gave the engine for it (i = NVN_NC_*).
enum { NVN_NC_HANDLE, NVN_NC_BIND, NVN_NC_DRAW, NVN_NC_DRAWI, NVN_NC_REN, NVN_NC_N };
int nvn_nc_ready(const void *thunk[NVN_NC_N]);
// The bodies the calls of the engine reach, called directly (no thunk, no call counter, no wrapper). Run them on the big stack.
void nvn_nc_bind(uint64_t cb, int stage, int slot, int32_t tex, int32_t smp);   // GetTextureHandle(tex, smp) + BindTexture(cb, stage, slot, handle)
void nvn_nc_ren(uint64_t cb);                                                   // SetRenderEnable(cb, 1)
void nvn_nc_draw(uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t instances);   // instances < 0: DrawElements
void *nvn_on_bigstack(void *(*fn)(void *), void *arg);                          // nvn_entry.S: fn(arg) on the host stack of this thread
// DM_NR_NOCALL=2. NVN calls this thread made through the big-stack wrapper (every implemented entry point) since it started.
uint64_t nvn_nc_thread_calls(void);
// The capture of this thread: what = 1 the BindTexture calls (a texture flush), 2 the draw + SetRenderEnable calls (an engine draw).
enum { NVN_NC_CAP_BINDS = 96 };
typedef struct {
    int nbind, ndraw, nren;                 // calls seen (binds past NVN_NC_CAP_BINDS are counted, not kept)
    struct { uint64_t cb, handle; int32_t stage, slot; } b[NVN_NC_CAP_BINDS];
    uint64_t dcb, ib, rcb; int32_t prim, itype, count, instances, ren_on;   // the last draw (instances -1 = DrawElements, -2 = a base vertex / instance) / SetRenderEnable
} nvn_nc_cap;
void nvn_nc_cap_begin(int what);
const nvn_nc_cap *nvn_nc_cap_end(int what);
uint64_t nvn_nc_handle(int32_t tex, int32_t smp);   // the value of GetTextureHandle
// ns: [0] an NVN call through its thunk (+ the big-stack wrapper) with an empty body, [1] the empty body called directly,
// [2] one nvn_on_bigstack round trip. On the calling thread (call it from the thread that records). 0 = no no-op entry point.
int nvn_nc_calibrate(double out_ns[3]);
#endif
