// [DOS2] NR-FRAME host test (docs/dos2/nr/NR-FRAME.md): nr_frame.h - stored components of the Vulkan colour formats against the
// spec's enum names, the format-aware write-mask rule (DM_NR_COVER_FMASK), and the rule on the game's real post pipelines (blend /
// write mask / colour format of every engine post shader in the owner's warm-up list play-1118/vk-pipeline-list.bin, decoded with
// tools/dos2_pipe_seed.py: fixed-function numbers only).
// Build (WSL): gcc -std=c11 -O2 -Wall -Wextra -I<tools>/src/mesa-full/include nr_frame_test.c -o /tmp/nr_frame_test && /tmp/nr_frame_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "../../../nr/nr_frame.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { VkFormat f; unsigned comp; const char *name; } fc;
static const fc formats[] = {
    {VK_FORMAT_R8_UNORM, 1, "R8_UNORM"}, {VK_FORMAT_R8_SNORM, 1, "R8_SNORM"}, {VK_FORMAT_R8_UINT, 1, "R8_UINT"}, {VK_FORMAT_R8_SRGB, 1, "R8_SRGB"},
    {VK_FORMAT_R16_UNORM, 1, "R16_UNORM"}, {VK_FORMAT_R16_SFLOAT, 1, "R16_SFLOAT"}, {VK_FORMAT_R16_SINT, 1, "R16_SINT"},
    {VK_FORMAT_R32_UINT, 1, "R32_UINT"}, {VK_FORMAT_R32_SINT, 1, "R32_SINT"}, {VK_FORMAT_R32_SFLOAT, 1, "R32_SFLOAT"},
    {VK_FORMAT_R8G8_UNORM, 2, "R8G8_UNORM"}, {VK_FORMAT_R8G8_SRGB, 2, "R8G8_SRGB"}, {VK_FORMAT_R16G16_UNORM, 2, "R16G16_UNORM"},
    {VK_FORMAT_R16G16_SFLOAT, 2, "R16G16_SFLOAT"}, {VK_FORMAT_R32G32_UINT, 2, "R32G32_UINT"}, {VK_FORMAT_R32G32_SFLOAT, 2, "R32G32_SFLOAT"},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 3, "B10G11R11_UFLOAT_PACK32"}, {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 3, "E5B9G9R9_UFLOAT_PACK32"},
    {VK_FORMAT_R5G6B5_UNORM_PACK16, 3, "R5G6B5_UNORM_PACK16"}, {VK_FORMAT_B5G6R5_UNORM_PACK16, 3, "B5G6R5_UNORM_PACK16"},
    {VK_FORMAT_R8G8B8A8_UNORM, 4, "R8G8B8A8_UNORM"}, {VK_FORMAT_R8G8B8A8_SRGB, 4, "R8G8B8A8_SRGB"}, {VK_FORMAT_B8G8R8A8_UNORM, 4, "B8G8R8A8_UNORM"},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, "A2B10G10R10_UNORM_PACK32"}, {VK_FORMAT_R16G16B16A16_SFLOAT, 4, "R16G16B16A16_SFLOAT"},
    {VK_FORMAT_R32G32B32A32_SFLOAT, 4, "R32G32B32A32_SFLOAT"}, {VK_FORMAT_R4G4B4A4_UNORM_PACK16, 4, "R4G4B4A4 (unknown -> 4)"},
    {VK_FORMAT_R8G8B8_UNORM, 4, "R8G8B8_UNORM (not in the table -> 4, conservative)"}, {VK_FORMAT_UNDEFINED, 4, "UNDEFINED -> 4"},
    {VK_FORMAT_D16_UNORM, 0, "D16"}, {VK_FORMAT_X8_D24_UNORM_PACK32, 0, "X8_D24"}, {VK_FORMAT_D32_SFLOAT, 0, "D32"}, {VK_FORMAT_S8_UINT, 0, "S8"},
    {VK_FORMAT_D16_UNORM_S8_UINT, 0, "D16S8"}, {VK_FORMAT_D24_UNORM_S8_UINT, 0, "D24S8"}, {VK_FORMAT_D32_SFLOAT_S8_UINT, 0, "D32S8"},
};

// The engine post pipelines of the owner's warm-up list (play-1118, Mi 14 Ultra): fs key, name, Vulkan colour format, blend enable,
// write mask, depth / stencil used. One row per colour attachment of each distinct pipeline.
typedef struct { const char *key, *name; unsigned vk, blend, mask, ds; } pf;
static const pf pipes[] = {
    {"010472595b342188", "EoCOutlineCombine", 37, 0, 0xf, 0},
    {"0e3fc07b320db1e9", "PPBloomDownsampleAndBlur", 122, 0, 0x7, 0},
    {"1372db843749e1d3", "CascadedPCF_3x768", 9, 0, 0x1, 0},
    {"2a221ee245d55aab", "CascadedPCF_3x512", 9, 0, 0x1, 0},
    {"343488601475d44d", "CrossBilateralUpsample", 122, 1, 0x7, 0},
    {"43e8e8ed2b1df6c2", "PPVignette", 37, 0, 0xf, 0},
    {"4e290c9cf1162fe9", "PPBloomUpsampleAndBlur", 122, 0, 0x7, 0},
    {"5e26ce730b83399a", "Iggy blur", 37, 0, 0xf, 0},
    {"62d683cc49953ad6", "FadeBlending", 122, 0, 0x7, 0},
    {"640a4c497bcc748b", "DeferredSky", 122, 1, 0x7, 0},
    {"691768aa08545d95", "PPFXAA", 37, 0, 0xf, 0},
    {"74fe9ae2066d53da", "PPBloomScaleBiasDownsampleAndBlur", 122, 0, 0x7, 0},
    {"7b3210191abd90cd", "PPDefault (R11G11B10F)", 122, 0, 0x7, 0},
    {"7b3210191abd90cd", "PPDefault (RGBA8)", 37, 0, 0xf, 0},
    {"91eb2d97a304709a", "EoCSight", 37, 0, 0xf, 0},
    {"921728a5bbb1c71b", "PPApplyLUT", 37, 0, 0xf, 0},
    {"98186c22a7209861", "CombineUI_NoFetch", 37, 0, 0xf, 0},
    {"b5afd82fadffd7db", "Iggy fill", 37, 0, 0xf, 0},
    {"cbe8649300e9c8cd", "PPDownSampleLog2", 122, 0, 0x7, 0},
    {"d236b8d5b7ffad3c", "ClusteredLightingWithSSAO", 122, 1, 0x7, 0},
    {"d8aeec640d465275", "DeferredFog", 122, 0, 0x7, 0},
    {"d99f1fdb55bbafd0", "Iggy", 37, 1, 0xf, 0},
    {"e94ea226e689801c", "EoCFog", 37, 0, 0xf, 0},
    {"eaaa50c5ddbfd31b", "G-buffer material c0", 122, 0, 0x7, 1},
    {"eaaa50c5ddbfd31b", "G-buffer material c1", 83, 0, 0x3, 1},
    {"f23c09d198b55dc5", "LinearizeDepth", 70, 0, 0x1, 0},
    {"f3fd8e1015789f30", "LuminanceAccum", 76, 0, 0x1, 0},
};

// xbg_nr_cover.inc decide(): the attachment test of one colour slot (blend off, mask), old rule and DM_NR_COVER_FMASK=1
static int old_rule(const pf *p) { return !p->blend && p->mask == 15 && !p->ds; }
static int new_rule(const pf *p) { return !p->blend && (p->mask == 15 || nr_fr_mask_full(p->mask, nr_fr_vk_components(p->vk))) && !p->ds; }

int main(void) {
    // 1. the component table against the spec's enum names (a wrong number in nr_frame.h would show here)
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++)
        CHECK(nr_fr_vk_components((uint32_t)formats[i].f) == formats[i].comp, "%s (%d): %u components, expected %u", formats[i].name,
              (int)formats[i].f, nr_fr_vk_components((uint32_t)formats[i].f), formats[i].comp);
    // 2. the mask rule: every mask x every component count
    for (unsigned comp = 0; comp <= 4; comp++)
        for (unsigned m = 0; m < 16; m++) {
            const unsigned need = comp >= 4 ? 15u : (1u << comp) - 1u;
            const int want = comp != 0 && (m & need) == need;
            CHECK(nr_fr_mask_full(m, comp) == want, "mask %#x comp %u: %d, expected %d", m, comp, nr_fr_mask_full(m, comp), want);
        }
    CHECK(nr_fr_mask_full(0xf, 4) && !nr_fr_mask_full(0x7, 4), "RGBA needs 0xF");
    CHECK(nr_fr_mask_full(0x7, 3) && !nr_fr_mask_full(0x3, 3) && !nr_fr_mask_full(0x6, 3), "R11G11B10F: 0x7 full, 0x3 / 0x6 not");
    CHECK(nr_fr_mask_full(0x1, 1) && nr_fr_mask_full(0xf, 1) && !nr_fr_mask_full(0x2, 1), "R8 / R16: bit 0 alone decides");
    CHECK(!nr_fr_mask_full(0xf, 0), "a depth format is never a full colour write");
    // 3. the rule on the game's pipelines: the old rule never accepts more (superset), and which shaders it gains
    const char *gain[] = {"0e3fc07b320db1e9", "1372db843749e1d3", "2a221ee245d55aab", "4e290c9cf1162fe9", "62d683cc49953ad6", "74fe9ae2066d53da",
                          "7b3210191abd90cd", "cbe8649300e9c8cd", "d8aeec640d465275", "f23c09d198b55dc5", "f3fd8e1015789f30"};
    unsigned gained = 0, still_blend = 0, kept = 0;
    for (size_t i = 0; i < sizeof pipes / sizeof *pipes; i++) {
        const pf *p = &pipes[i];
        CHECK(!old_rule(p) || new_rule(p), "%s: the new rule refuses what the old one accepted", p->name);
        if (new_rule(p) && !old_rule(p)) {
            int listed = 0;
            for (size_t g = 0; g < sizeof gain / sizeof *gain; g++) if (!strcmp(gain[g], p->key)) listed = 1;
            CHECK(listed, "%s gains unexpectedly", p->name);
            ++gained;
            if (getenv("NRFR_SHOW")) printf("  gains: %s %s (vk %u, mask %#x)\n", p->key, p->name, p->vk, p->mask);
        }
        if (p->blend) { ++still_blend; CHECK(!new_rule(p), "%s blends: never covered", p->name); }
        if (old_rule(p)) ++kept;
    }
    CHECK(gained == 11, "11 pipeline rows gain (one per post shader on R11G11B10F / R8 / R16 / R16F), got %u", gained);
    CHECK(still_blend == 4, "Sky, Light, CrossBilateralUpsample, Iggy stay refused (blend), got %u rows", still_blend);
    CHECK(kept == 10, "the 10 RGBA8 0xF rows were accepted before and still are, got %u", kept);
    // the G-buffer keeps its refusal through depth (its masks are full for the format: the attachment test alone would pass)
    CHECK(nr_fr_mask_full(0x3, nr_fr_vk_components(83)) && !new_rule(&pipes[24]), "G-buffer RG16F 0x3 is full, refused by depth");
    printf("nr_frame_test: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
