// [DOS2] NR-UBWC host test (docs/dos2/nr/NR-UBWC.md): dm/gfx/nr/nr_ubwc.h - Turnip 1c3a936's UBWC rule - against the Vulkan enum names,
// then on DOS2's render targets (formats / sizes from the frame table, docs/dos2/nr/NR-FRAME.md) for five Adreno generations.
// Build (WSL): gcc -std=c11 -O2 -Wall -Wextra -I<tools>/src/mesa-full/include nr_ubwc_test.c -o /tmp/nr_ubwc_test && /tmp/nr_ubwc_test
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan_core.h>
#include "../../../nr/nr_ubwc.h"

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static nr_ubwc_img img(VkFormat f, uint32_t w, uint32_t h, uint8_t storage) {
    nr_ubwc_img m = {(uint32_t)f, w, h, 1, 1, 1, 2, 0, storage, 0, 0, 0, 1}; return m;
}
// mb: bytes the frame writes + reads of the target (model of NR-FRAME 1.1, 2160x972 3D, 2400x1080 window)
typedef struct { const char *name; VkFormat f; uint32_t w, h; double mb; } tgt;
static const tgt dos2[] = {
    {"G-buffer c0 (emissive)", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 2160, 972, 16.8}, {"G-buffer c1 (normal)", VK_FORMAT_R16G16_SFLOAT, 2160, 972, 16.8},
    {"G-buffer c2", VK_FORMAT_R8G8B8A8_UNORM, 2160, 972, 16.8}, {"G-buffer c3", VK_FORMAT_R8G8B8A8_UNORM, 2160, 972, 16.8},
    {"3D depth", VK_FORMAT_D24_UNORM_S8_UINT, 2160, 972, 33.6}, {"linear depth", VK_FORMAT_R16_UNORM, 2160, 972, 21.0},
    {"shadow mask (PCF)", VK_FORMAT_R8_UNORM, 2160, 972, 4.2}, {"Sky Sp / lit Df", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 2160, 972, 67.2},
    {"bloom 1080", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 1080, 486, 8.4}, {"luminance 256", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 256, 256, 0.5},
    {"luminance 8x8", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 8, 8, 0.0}, {"exposure 1x1", VK_FORMAT_R16_SFLOAT, 1, 1, 0.0},
    {"particles 1/2", VK_FORMAT_R16G16B16A16_SFLOAT, 1080, 486, 8.4}, {"post RGBA8", VK_FORMAT_R8G8B8A8_UNORM, 2160, 972, 50.4},
    {"UI / window", VK_FORMAT_R8G8B8A8_UNORM, 2400, 1080, 41.5}, {"shadow atlas", VK_FORMAT_D16_UNORM, 1536, 1536, 9.4},
};
int main(void) {
    // 1. the format facts against the enum names
    CHECK(nru_cpp(VK_FORMAT_R8_UNORM) == 1 && nru_cpp(VK_FORMAT_R8_SRGB) == 1 && nru_cpp(VK_FORMAT_R16_SFLOAT) == 2 && nru_cpp(VK_FORMAT_D16_UNORM) == 2, "cpp 1 / 2");
    CHECK(nru_cpp(VK_FORMAT_R8G8B8_UNORM) == 3 && nru_cpp(VK_FORMAT_B8G8R8_SRGB) == 3 && nru_cpp(VK_FORMAT_R16G16B16_SFLOAT) == 6 && nru_cpp(VK_FORMAT_R32G32B32_SFLOAT) == 12, "3-component cpp");
    CHECK(nru_cpp(VK_FORMAT_R16G16B16A16_SFLOAT) == 8 && nru_cpp(VK_FORMAT_R32G32_SFLOAT) == 8 && nru_cpp(VK_FORMAT_R32G32B32A32_SFLOAT) == 16, "8 / 16");
    CHECK(nru_cpp(VK_FORMAT_B10G11R11_UFLOAT_PACK32) == 4 && nru_cpp(VK_FORMAT_D24_UNORM_S8_UINT) == 4 && nru_cpp(VK_FORMAT_R16G16_SFLOAT) == 4, "cpp 4");
    const VkFormat sn[] = {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_B8G8R8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_B8G8R8A8_SNORM,
        VK_FORMAT_A8B8G8R8_SNORM_PACK32, VK_FORMAT_A2R10G10B10_SNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM,
        VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM};
    for (unsigned i = 0; i < sizeof sn / sizeof *sn; i++) CHECK(nru_is_snorm(sn[i]), "snorm %d", (int)sn[i]);
    CHECK(!nru_is_snorm(VK_FORMAT_R8G8B8A8_UNORM) && !nru_is_snorm(VK_FORMAT_A8B8G8R8_UNORM_PACK32) && !nru_is_snorm(VK_FORMAT_A2B10G10R10_UNORM_PACK32), "unorm is not snorm");
    CHECK(nru_is_compressed(VK_FORMAT_BC1_RGB_UNORM_BLOCK) && nru_is_compressed(VK_FORMAT_ASTC_12x12_SRGB_BLOCK) && !nru_is_compressed(VK_FORMAT_D32_SFLOAT_S8_UINT), "compressed range");
    // 2. the rule's corners
    const nr_ubwc_dev a750 = nr_ubwc_dev_a7xx_gen3(), a740 = nr_ubwc_dev_a7xx_gen2(), a730 = nr_ubwc_dev_a7xx_gen1(), a650 = nr_ubwc_dev_a6xx_gen3(), a630 = nr_ubwc_dev_a6xx_gen1();
    nr_ubwc_img m = img(VK_FORMAT_R8G8B8A8_UNORM, 2160, 972, 1);
    CHECK(nr_ubwc_turnip(&a750, &m) == NRU_ON && nr_ubwc_turnip(&a730, &m) == NRU_ON, "a7xx: STORAGE keeps UBWC");
    CHECK(nr_ubwc_turnip(&a650, &m) == NRU_STORAGE && nr_ubwc_turnip(&a630, &m) == NRU_STORAGE, "a6xx: STORAGE drops UBWC");
    m = img(VK_FORMAT_R8_UNORM, 2160, 972, 0);
    CHECK(nr_ubwc_turnip(&a750, &m) == NRU_8BPP && nr_ubwc_turnip(&a650, &m) == NRU_8BPP && nr_ubwc_turnip(&a630, &m) == NRU_ON, "8-bit: only a630 compresses");
    m = img(VK_FORMAT_R8G8B8A8_SNORM, 64, 64, 0);
    CHECK(nr_ubwc_turnip(&a740, &m) == NRU_ON && nr_ubwc_turnip(&a730, &m) == NRU_SNORM, "snorm from a740 on");
    m = img(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0); m.mutable_fmt = 1;
    CHECK(nr_ubwc_turnip(&a750, &m) == NRU_MUTABLE, "mutable without a list"); m.fmt_list_compatible = 1; CHECK(nr_ubwc_turnip(&a750, &m) == NRU_ON, "mutable with a list");
    m = img(VK_FORMAT_R8G8B8A8_UNORM, 15, 64, 0); CHECK(nr_ubwc_turnip(&a750, &m) == NRU_NARROW, "narrow: linear fallback");
    m = img(VK_FORMAT_R8G8B8_UNORM, 64, 64, 0); CHECK(nr_ubwc_turnip(&a750, &m) == NRU_CPP, "RGB8: cpp 3");
    m = img(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 64, 64, 0); CHECK(nr_ubwc_turnip(&a750, &m) == NRU_E5B9G9R9_S8, "E5B9G9R9");
    m = img(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0); m.type = 3; m.levels = 2; CHECK(nr_ubwc_turnip(&a750, &m) == NRU_3D_MIPS, "3D + mips");
    m = img(VK_FORMAT_D24_UNORM_S8_UINT, 64, 64, 0); m.stencil_sampled = 1; CHECK(nr_ubwc_turnip(&a750, &m) == NRU_ON, "D24S8 sampled stencil with z24uint_s8uint");
    m.use_z24uint_s8uint = 0; CHECK(nr_ubwc_turnip(&a750, &m) == NRU_D24S8_STENCIL, "ANGLE workaround");
    // 3. DOS2's targets: what loses UBWC where (no STORAGE / STORAGE on every colour target = the worst case of NVN IMAGE flags)
    const nr_ubwc_dev *devs[] = {&a750, &a740, &a730, &a650, &a630}; const char *dn[] = {"A750 (Mi 14)", "A740", "A730", "A650/A660", "A630"};
    double off_mb[5][2] = {{0}}, all = 0;
    for (unsigned i = 0; i < sizeof dos2 / sizeof *dos2; i++) all += dos2[i].mb;
    for (unsigned d = 0; d < 5; d++)
        for (unsigned st = 0; st < 2; st++)
            for (unsigned i = 0; i < sizeof dos2 / sizeof *dos2; i++) {
                const int depth = dos2[i].f == VK_FORMAT_D24_UNORM_S8_UINT || dos2[i].f == VK_FORMAT_D16_UNORM;
                nr_ubwc_img q = img(dos2[i].f, dos2[i].w, dos2[i].h, (uint8_t)(st && !depth));
                const int r = nr_ubwc_turnip(devs[d], &q);
                if (r != NRU_ON) off_mb[d][st] += dos2[i].mb;
                if (getenv("NRU_SHOW") && r != NRU_ON) printf("  %-13s %s STORAGE: %-24s %s\n", dn[d], st ? "with" : "no  ", dos2[i].name, nru_name[r]);
            }
    for (unsigned d = 0; d < 5; d++)
        printf("  %-13s uncompressed target traffic (model, of %.0f MB): %.1f MB a frame without STORAGE, %.1f MB with STORAGE on every colour target\n", dn[d], all, off_mb[d][0], off_mb[d][1]);
    CHECK(off_mb[0][0] > 4.19 && off_mb[0][0] < 4.21 && off_mb[0][1] > 4.19 && off_mb[0][1] < 4.21, "A750: only the R8 shadow mask (+ the <16 px luminance tail, ~0 MB) is uncompressed, STORAGE or not");
    CHECK(off_mb[3][1] > 250.0, "A650: STORAGE on the colour targets leaves most of the frame uncompressed");
    printf("nr_ubwc_test: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
