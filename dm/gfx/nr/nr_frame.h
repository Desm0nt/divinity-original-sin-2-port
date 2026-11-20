// [DOS2] NR-FRAME (docs/dos2/nr/NR-FRAME.md): pure parts of the frame-restructure step. Pure C, no Vulkan headers (the Vulkan format
// numbers are the spec's enum values, checked against vulkan_core.h by tests/nr_frame_test.c), shared by the backend
// (xbg_nr_cover.inc) and the host test.
//
// nr_fr_vk_components: how many colour components a Vulkan colour format STORES (R = 1, RG = 2, RGB / B10G11R11 / E5B9G9R9 = 3,
// RGBA / BGRA / A2B10G10R10 = 4). 0 for depth / stencil formats; unknown formats answer 4 (the conservative value: the full 0xF mask
// is needed). A colour write mask bit of a component the format does not have writes nothing (Vulkan: components not present in the
// attachment format are discarded), so a mask that covers every component the format has is a full overwrite of every texel the
// draw touches: the old texel cannot survive in it.
// nr_fr_mask_full: the 4-bit write mask covers every stored component of a format with `components` components.
// Rule used by DM_NR_COVER_FMASK=1 (NR-10b's proof "the first draw writes every byte of the attachment"): the old rule asked for
// mask == 0xF, which refused the game's R11G11B10F (mask 0x7), R8 / R16 / R16F (0x1) post passes although they overwrite in full.
#ifndef DM_GFX_NR_FRAME_H
#define DM_GFX_NR_FRAME_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

static inline unsigned nr_fr_vk_components(uint32_t f) {
    switch (f) {
    // 8-bit and 16-bit single-channel (R4G4 / R5G6B5 etc. are not render targets here: they fall to the default)
    case 9: case 10: case 11: case 12: case 13: case 14: case 15:            // R8_UNORM .. R8_SRGB
    case 70: case 71: case 72: case 73: case 74: case 75: case 76:           // R16_UNORM .. R16_SFLOAT
    case 98: case 99: case 100:                                              // R32_UINT / SINT / SFLOAT
        return 1;
    case 16: case 17: case 18: case 19: case 20: case 21: case 22:           // R8G8_*
    case 77: case 78: case 79: case 80: case 81: case 82: case 83:           // R16G16_*
    case 101: case 102: case 103:                                            // R32G32_*
        return 2;
    case 122:                                                                // B10G11R11_UFLOAT_PACK32
    case 123:                                                                // E5B9G9R9_UFLOAT_PACK32
    case 4:                                                                  // R5G6B5_UNORM_PACK16
    case 5:                                                                  // B5G6R5_UNORM_PACK16
        return 3;
    case 124: case 125: case 126: case 127: case 128: case 129: case 130:    // D16 / X8_D24 / D32 / S8 / D16S8 / D24S8 / D32S8
        return 0;
    default:
        return 4;                                                            // RGBA / BGRA / A2B10G10R10 / RGBA16 / RGBA32 / anything else
    }
}
static inline int nr_fr_mask_full(unsigned mask4, unsigned components) {
    const unsigned need = components >= 4 ? 15u : (1u << components) - 1u;
    return components != 0 && (mask4 & need) == need;
}

#ifdef __cplusplus
}
#endif
#endif
