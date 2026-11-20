// [DOS2] VK-NATIVE stage 4, texture read-back (vkn_read.c): the CPU read of a texture's texels the engine does with
// nvnTextureReadTexels (screenshots, the save-game thumbnail), on the backend's images. No game addresses.
// What NVN returns (and so what this returns): the texel bytes of the texture's NVN format, row 0 = y 0 (the backend renders with
// the NVN row order: its viewport is flipped, so image row 0 is NVN row 0), rows `pitch` bytes apart (0 = tight: width * bytes
// per texel, what ReadTexels without a stride writes). Widened textures (RGB stored as RGBA) give their 3 components back.
// Not readable (vkn_read_supported says why): BC / ASTC (native or CPU-decoded: the original blocks are gone), depth / stencil,
// multisampled, 3-byte-unaligned formats the device widened differently, unknown formats.
// How: the copy goes into the open OFF-FRAME recording if there is one (the engine's off-frame screenshot recording that nothing
// submitted), else into an off-frame recording of its own (vkn_rec_begin(0)); then vkn_rec_submit(1) (submit and wait) and the copy
// out of the staging block. A presented recording open = refused (its submit is the frame's; waiting would cut the frame).
// The recording thread only (vkn_frame.h is single-threaded): a call from another thread while a recording thread is known is refused.
#ifndef DM_VKN_READ_H
#define DM_VKN_READ_H
#include "vkn.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

enum {
    VKN_READ_OK = 0,
    VKN_READ_E_TEX = -1,       // NULL / destroyed texture
    VKN_READ_E_FMT = -2,       // format not readable (vkn_read_supported)
    VKN_READ_E_RECT = -3,      // empty / out-of-range rectangle, level, layer
    VKN_READ_E_BUSY = -4,      // a presented recording is open
    VKN_READ_E_THREAD = -5,    // not the recording thread
    VKN_READ_E_CB = -6,        // no device / the off-frame recording could not begin / the copy could not be recorded
    VKN_READ_E_MEM = -7,       // no staging memory
    VKN_READ_E_SUBMIT = -8,    // submit or wait failed (dst left untouched)
};
// Install time (mode 1): registers the begin listener that tells this module which thread records. Idempotent.
void vkn_read_init(void);
// 1 = the calling thread may read now: it is the recording thread (or none is known yet) and no presented recording is open.
int vkn_read_now_ok(void);
// 1 = t can be read; 0 = not (*why: a short static reason).
int vkn_read_supported(const vkn_tex *t, const char **why);
// Bytes per texel of t's NVN format as ReadTexels writes them (0 = not a 1x1-block format).
uint32_t vkn_read_texel_bytes(const vkn_tex *t);
// Reads the rectangle (x, y, w, h) of (level, layer) of t into dst (layer: array layer / cube face; for a 3D texture the slice z).
// A texture the GPU never wrote and no upload filled (layout UNDEFINED after the queued uploads ran) reads as zeros (the backend's
// fresh image memory is garbage; the engine's pool memory is zeroed). *wait_ns (may be NULL): the time spent in the submit + wait.
// Returns VKN_READ_OK or a VKN_READ_E_* (dst untouched on every error).
int vkn_read_texels(vkn_tex *t, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t w, uint32_t h, void *dst, uint64_t pitch, uint64_t *wait_ns);

typedef struct vkn_read_stats {
    uint64_t reads, bytes, wait_ns, own_rec, engine_rec, undefined_zero;
    uint64_t e_tex, e_fmt, e_rect, e_busy, e_thread, e_cb, e_mem, e_submit;
} vkn_read_stats;
void vkn_read_stats_get(vkn_read_stats *s);

#ifdef __cplusplus
}
#endif
#endif
