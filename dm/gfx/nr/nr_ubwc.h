// [DOS2] NR-UBWC (docs/dos2/nr/NR-UBWC.md): Turnip's UBWC decision for one VkImageCreateInfo, as the bundled driver turnip27 = Mesa
// 1c3a936 makes it (tu_image.cc ubwc_possible + tu_image_init, fd6_layout.c fdl6_layout_image, freedreno_devices.py props). Pure C, no
// Vulkan header (format numbers = the spec's enum values). The runtime does not need it - it asks the driver (VK_EXT_image_compression_
// control, xbg_nr_ubwc.inc); this is the offline model behind the docs table and the host test tests/nr_ubwc_test.c (which DOS2 target
// loses UBWC on which Adreno generation, and why). Not modelled (DOS2 never uses them): sparse, host-transfer usage, external / AHB
// memory with a linear modifier, fragment density maps, YUV, VK_IMAGE_COMPRESSION_DISABLED_EXT, TU_DEBUG=noubwc.
#ifndef DM_GFX_NR_UBWC_H
#define DM_GFX_NR_UBWC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The props of freedreno_devices.py that decide it (A6xxGPUInfo templates; a prop a template does not set is false)
typedef struct nr_ubwc_dev {
    uint8_t is_a702, supports_uav_ubwc, has_8bpp_ubwc, ubwc_unorm_snorm_int_compatible, ubwc_all_formats_compatible, has_z24uint_s8uint;
    uint8_t linear_threshold_in_blocks;   // supports_linear_mipmap_threshold_in_blocks (a7xx base)
} nr_ubwc_dev;
// a6xx_base sets has_8bpp_ubwc (a630 = gen1 keeps it); gen2..gen4 clear it; a7xx_base never sets it; a7xx_gen1..3 set supports_uav_ubwc,
// gen2 / gen3 ubwc_unorm_snorm_int_compatible, gen3 ubwc_all_formats_compatible.
static inline nr_ubwc_dev nr_ubwc_dev_a6xx_gen1(void) { nr_ubwc_dev d = {0, 0, 1, 0, 0, 1, 0}; return d; }   // A630 (SD 845)
static inline nr_ubwc_dev nr_ubwc_dev_a6xx_gen3(void) { nr_ubwc_dev d = {0, 0, 0, 0, 0, 1, 0}; return d; }   // A650 / A660 / A642 class
static inline nr_ubwc_dev nr_ubwc_dev_a7xx_gen1(void) { nr_ubwc_dev d = {0, 1, 0, 0, 0, 1, 1}; return d; }   // A730 / A720 / A710
static inline nr_ubwc_dev nr_ubwc_dev_a7xx_gen2(void) { nr_ubwc_dev d = {0, 1, 0, 1, 0, 1, 1}; return d; }   // A740 / A735
static inline nr_ubwc_dev nr_ubwc_dev_a7xx_gen3(void) { nr_ubwc_dev d = {0, 1, 0, 1, 1, 1, 1}; return d; }   // A750 (Mi 14 Ultra)

typedef struct nr_ubwc_img {
    uint32_t vk_format, width, height, depth, levels, samples;
    uint8_t type;            // 1 = 1D, 2 = 2D, 3 = 3D
    uint8_t tiling_linear, storage, mutable_fmt, fmt_list_compatible;   // fmt_list_compatible: a VkImageFormatListCreateInfo of UBWC-compatible formats
    uint8_t stencil_sampled; // D24S8 with SAMPLED / INPUT_ATTACHMENT stencil usage
    uint8_t use_z24uint_s8uint;   // device->use_z24uint_s8uint (true unless the ANGLE border-colour drirc workaround)
} nr_ubwc_img;

enum { NRU_ON = 0, NRU_A702, NRU_COMPRESSED_FMT, NRU_E5B9G9R9_S8, NRU_SNORM, NRU_8BPP, NRU_3D_MIPS, NRU_STORAGE, NRU_D24S8_STENCIL,
       NRU_LINEAR, NRU_1D, NRU_MUTABLE, NRU_CPP, NRU_NARROW, NRU_N };
static const char *const nru_name[NRU_N] = {"UBWC", "a702", "block-compressed format", "E5B9G9R9 / S8", "SNORM before a740", "8-bit format (no has_8bpp_ubwc)",
    "3D with mips", "STORAGE usage (no supports_uav_ubwc)", "D24S8 with sampled stencil", "linear tiling", "1D image", "MUTABLE_FORMAT without a compatible list",
    "bytes per pixel not a power of two", "narrower than the linear fallback (16 texels)"};

// Vulkan format facts the rule needs (by number): bytes per texel, block compressed, snorm
static inline int nru_is_compressed(uint32_t f) { return (f >= 131 && f <= 184) || (f >= 1000054000u && f <= 1000054007u) || (f >= 1000066000u && f <= 1000066013u); }
static inline int nru_is_snorm(uint32_t f) {
    return f == 10 || f == 17 || f == 24 || f == 31 || f == 38 || f == 45 || f == 52 || f == 59 || f == 65 || f == 71 || f == 78 || f == 85 || f == 92; }
static inline uint32_t nru_cpp(uint32_t f) {
    if (f >= 9 && f <= 15) return 1;
    if ((f >= 16 && f <= 22) || (f >= 70 && f <= 76) || f == 124 || (f >= 2 && f <= 8)) return 2;
    if ((f >= 23 && f <= 36) || (f >= 84 && f <= 90) || (f >= 104 && f <= 106)) return f >= 104 ? 12u : f >= 84 ? 6u : 3u;   // RGB8 / BGR8 / RGB16 / RGB32
    if ((f >= 91 && f <= 97) || (f >= 101 && f <= 103) || (f >= 110 && f <= 112)) return 8;
    if (f >= 107 && f <= 109) return 16;
    return 4;   // RGBA8 / BGRA8 / A2B10G10R10 / RG16 / R32 / B10G11R11 / E5B9G9R9 / D24S8 / X8D24 / D32
}
static inline int nr_ubwc_turnip(const nr_ubwc_dev *d, const nr_ubwc_img *m) {
    const uint32_t f = m->vk_format;
    // tu_image_init: forced linear tiles first, then ubwc_possible in its order, then mutable, then fd6_layout
    if (m->tiling_linear) return NRU_LINEAR;
    if (m->type == 1) return NRU_1D;
    if (d->is_a702) return NRU_A702;
    if (nru_is_compressed(f)) return NRU_COMPRESSED_FMT;
    if (f == 123 || f == 127) return NRU_E5B9G9R9_S8;
    if (nru_is_snorm(f) && !d->ubwc_unorm_snorm_int_compatible) return NRU_SNORM;
    if (!d->has_8bpp_ubwc && nru_cpp(f) == 1) return NRU_8BPP;
    if (m->type == 3 && m->levels > 1) return NRU_3D_MIPS;
    if (m->storage && !d->supports_uav_ubwc) return NRU_STORAGE;
    if (!m->use_z24uint_s8uint && f == 129 && m->stencil_sampled) return NRU_D24S8_STENCIL;
    if (m->mutable_fmt && !(f >= 124 && f <= 130) && !m->fmt_list_compatible) return NRU_MUTABLE;   // a7xx_gen3 too: no list = format_list_ubwc_possible false
    const uint32_t cpp = nru_cpp(f) * (m->samples > 1 ? m->samples : 1u);
    if (cpp & (cpp - 1)) return NRU_CPP;
    if (m->width < 16u) return NRU_NARROW;   // FDL_LINEAR_FALLBACK_THRESHOLD x block width (1 for uncompressed)
    return NRU_ON;
}

#ifdef __cplusplus
}
#endif
#endif
