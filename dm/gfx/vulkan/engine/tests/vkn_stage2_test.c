// [DOS2] VK-NATIVE stage 2 host test (docs/dos2/nr/VK-NATIVE.md §3, VK-NATIVE-STATUS.md stage 2): the five modules of dm/gfx/vkn
// (vkn_core.c, vkn_buf.c, vkn_tex.c, vkn_shader.c, vkn_state.c) compiled with -DDM_GAME_DOS2 against the mock of vkn_mock_vk.h (the
// Vulkan entry points, the front end's dm_vk_handles / queue lock, the shader library's vkn_shlib_* door of mode 1).
//   core     arena alloc / free / merge / reuse, alignment, dedicated chunks, retire delay of exactly 3 frame ends, the upload queue
//            (order, run, flush under the queue lock), handle tables (put / get / kill / generation replace);
//   buffers  create with / without init data, destroy (the block comes back only after the retire delay), replace, pseudo addresses,
//            copy-in, the constant ring rule of FlushShaderConstants 0x925740 (MAP-2 §2.1: align 256, wrap when head + size >= 0x1380000);
//            stage 4 (copies): staged updates in place / renamed (carry-over only inside the engine size, none for a whole-buffer
//            update, dynamic vs static counters), has_block without marking the buffer used;
//   mirror   vkn_mirror band hashes: full first diff, unchanged, single / adjacent / separate bands, the run limit, stride padding,
//            a short last band, generation / geometry change, forget, eviction of the least recently used region;
//   textures the engine format table (every format 0..62 against nr_formats of dm/gfx/nr/nr_reg.c, which tools/dos2_nr_check.py checks
//            against main.raw, and an NVN -> Vulkan table written here from the NVN enum), sRGB pairs, 2D / cube / array textures with
//            linear data, a packaged (block-linear) texture built by hand with the Tegra GOB formula, BC on a device without BC (CPU decode),
//            RGB widening, render / depth target usage, pool-id lookups, destroy, mip-reduction swap, samplers for every filter / wrap /
//            compare value (MAP-1 §3.2 and CreateSampler 0x921e40);
//   shared   render-target storage (vkn_tex_create_shared, flag 0x2000 of 0x91e2a0): one storage identity + one shape = one vkn_tex for
//            every name (lookups, layout, pass state), both destroy orders and by handle, hand-over of the object's identity, a handle
//            index taken by a newer texture, size / format / data kept separate by reason, other identities and plain creates never
//            attach, a resize generation on the same reservation, an engine object reused while attached;
//   ASTC     stage 4, the setup pack (vkn_astc.c) on a device without BC: a synthetic D3AC pack written here (hit with every level and the
//            zero-block fix, the sRGB twin, 6x6 with two levels, key miss with an entry of another byte count, level count in the key /
//            shape filter without hashing, block size the device lacks, linear / BC4 / cube never looked up, an update refused, the mip
//            reduction re-made as ASTC + its tail copy, no pack / corrupt entry / bad magic / DM_VK_NATIVE_ASTC=0, a short read);
//   BC GPU   stage 4, the GPU routes after the pack (vkn_bcgpu.c, translator doors stubbed here): route 3 (auto on the mock's Qualcomm)
//            with its command sequence, staged region table + blocks and copies, decoder self-test once per format (a failed one -> CPU),
//            BC4 / BC6H on the CPU, route 2 (DM_GFX_BC_ASTC=1: decode + encode dispatches, ASTC 4x4 UNORM / sRGB, the encoder self-test with
//            the zero re-read and re-run, the family's both pipelines), an update of a GPU-ASTC texture through the GPU, the module's shape
//            rule (BC3 array) and image properties -> RGBA8, a failed encoder self-test / =0 / DM_GFX_BC_DECODE=cpu / no doors, bands of a
//            level bigger than a batch, the pack first and a mip reduction re-made from GPU ASTC 4x4 into the pack's 6x6;
//   state    blend factors / ops, depth-stencil funcs / ops, cull / fill (MAP-1 §3.2, 0x922530 / 0x922d10 / 0x923360; expected tables
//            written here from the doc's NVN numbers, not from the code), vertex formats (MAP-1 §3.1, 0x9181d0; the engine's own
//            fullscreen-mesh element list of 0x1f58698), the write-mask rule (0x9282c0, MAP-2), the primitive table 0x1f58640, the render
//            pass cache, framebuffers + forget-view, the pipeline key, the pipeline cache and the captured create info;
//   shaders  synthetic bundles through the mocked vkn_shlib_shader: set layout bindings (original x 6 + stage, uniform buffers dynamic by
//            the dynamic-offset policy), the binding table, c1 blocks, missing / unusable stages, variants through the export, a destroy
//            that cancels a queued creation, ids, the SUPPORT block.
// Ends with "vkn_stage2_test: N checks passed" (exit 0) or the failures and exit 1.
// Wiring the test does itself because no host file does it yet: vkn_shader_set_dead_fn(-> vkn_pipe_forget_shader) - without it a
// shader id reused after a destroy would hit pipelines built from the dead shader's modules (dead_to_pipes below).
//
// Build + run (WSL gcc: the vkn files use pthreads, Windows clang has no pthread.h). <vk> = a directory holding only vulkan/ and
// vk_video/ copied from <ndk>/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include (its libc headers are not usable on the host).
// From the repository root (one command line; wrapped here):
//   wsl gcc -std=c11 -D_GNU_SOURCE -DDM_GAME_DOOM=1 -DDM_GAME_D3=1 -DDM_GAME_TL2=1 -DDM_GAME_DOS2=1 -O1 -g -Wall -Wextra -Wno-unused-parameter -I<vk> -o /tmp/vkn_stage2_test
//       dm/gfx/vulkan/engine/tests/vkn_stage2_test.c dm/gfx/vkn/vkn_core.c dm/gfx/vkn/vkn_buf.c dm/gfx/vkn/vkn_tex.c
//       dm/gfx/vkn/vkn_shader.c dm/gfx/vkn/vkn_state.c dm/gfx/vkn/vkn_mirror.c dm/gfx/vkn/vkn_astc.c dm/gfx/vkn/vkn_bcgpu.c dm/gfx/vkn/vkn_drv.c dm/gfx/nvn/nvn_tex.c dm/gfx/tex/nvn_deswizzle.c dm/gfx/tex/nvn_bcdec.c
//       dm/gfx/nr/nr_reg.c -lpthread -lm && wsl /tmp/vkn_stage2_test
// (the game defines of the dos2 build, dm/android/CMakeLists.txt: nvn_tex.c's layout rules depend on DM_GAME_D3; -fsanitize=address,undefined
// works the same way.)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vkn_mock_vk.h"
#include "../../../vkn/vkn.h"
#include "../../../vkn/vkn_buf.h"
#include "../../../vkn/vkn_tex.h"
#include "../../../vkn/vkn_policy.h"   // [VK-DRV] vkn_policy_sim_nod24
#include "../../../vkn/vkn_astc.h"
#include "../../../vkn/vkn_bcgpu.h"
#include "../../../vkn/vkn_shader.h"
#include "../../../vkn/vkn_state.h"
#include "../../../vkn/vkn_mirror.h"
#include "../../../nvn/nvn_tex.h"
#include "../../../tex/nvn_deswizzle.h"
#include "../../../nr/nr_reg.h"

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { const long long a_ = (long long)(a), b_ = (long long)(b); g_checks++; \
    if (a_ != b_) { g_fails++; printf("FAIL %s:%d: %s == %s (%lld / %#llx vs %lld / %#llx)\n", __FILE__, __LINE__, #a, #b, a_, (unsigned long long)a_, b_, (unsigned long long)b_); } } while (0)
#define H(idx, gen) ((uint64_t)(gen) << 32 | (uint32_t)(idx))   // an engine handle: index in the low 32 bits, generation above

static void frames(int n) { for (int i = 0; i < n; i++) vkn_frame_end(); }
static uint64_t used_of(int cls) { uint64_t u[VKN_ARENAS], r[VKN_ARENAS]; vkn_arena_stats(u, r); return u[cls]; }
static uint64_t res_of(int cls) { uint64_t u[VKN_ARENAS], r[VKN_ARENAS]; vkn_arena_stats(u, r); return r[cls]; }

// ================================================================================================================ core
static int g_ret_runs[8];
static void ret_fn(void *ctx) { g_ret_runs[(intptr_t)ctx]++; }
static int g_up_order[16], g_up_n; static VkCommandBuffer g_up_cb[16];
static void up_fn(VkCommandBuffer cb, void *ctx) { if (g_up_n < 16) { g_up_cb[g_up_n] = cb; g_up_order[g_up_n++] = (int)(intptr_t)ctx; } }

static void test_no_device(void) {
    mk_dev_on = 0;
    CHECK(vkn_device() == NULL);
    vkn_block b; CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 64, 0, &b), -1);
    CHECK_EQ(vkn_mem_type(0x7, 0, 0), -1);
    vkn_block o; CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(1, 1), 64, 0, 0, NULL, &o), -1);
    const char *why = NULL; vkn_tex_desc d; memset(&d, 0, sizeof d); d.efmt = 4; d.nvn_target = 1; d.w = d.h = 4;
    CHECK(vkn_tex_create(H(1, 1), &d, NULL, &why) == NULL && why && !strcmp(why, "no device"));
    CHECK_EQ(vkn_sampler_create(H(1, 1), 1, 0, 0, 0, 0, 1, 0), -1);
    vkn_stage_src src[VKN_SRCS]; memset(src, 0, sizeof src);
    CHECK_EQ(vkn_shader_create(H(1, 1), src), -1);
    CHECK_EQ(vkn_upload_flush(), 0);
    CHECK_EQ(mk_created[MK_BUFFER] + mk_created[MK_IMAGE] + mk_created[MK_SAMPLER], 0);
    mk_dev_on = 1;
    const vkn_dev *dv = vkn_device();
    CHECK(dv != NULL);
    if (dv) { CHECK(dv->dev == MK_DEV && dv->phys == MK_PHYS && dv->queue == MK_QUEUE); CHECK_EQ(dv->family, 2); CHECK_EQ(dv->mem.memoryTypeCount, 3); }
    CHECK(vkn_device() == dv);
    // memory types: wanted flags, preferred flags tried first, bits respected
    CHECK_EQ(vkn_mem_type(0x7, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT), 1);
    CHECK_EQ(vkn_mem_type(0x5, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT), 2);
    CHECK_EQ(vkn_mem_type(0x7, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0);
    CHECK_EQ(vkn_mem_type(0x1, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0), -1);
}

static void test_arena(void) {
    const uint64_t buf0 = mk_created[MK_BUFFER];
    vkn_block a, b, c, d, e;
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 100, 0, &a), 0);
    CHECK_EQ(a.off, 0); CHECK_EQ(a.size, 112); CHECK(a.cpu != NULL); CHECK_EQ(a.cls, VKN_ARENA_STATIC);
    CHECK_EQ(mk_created[MK_BUFFER], buf0 + 1);
    mk_obj *cb = mk_obj_of((uint64_t)(uintptr_t)a.buf);
    CHECK(cb && cb->kind == MK_BUFFER && cb->size == 16u << 20);
    if (cb) {
        const VkBufferUsageFlags want = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        CHECK_EQ(cb->usage & want, want);
        mk_obj *m = mk_obj_of(cb->bound);
        CHECK(m && m->mem_type == 1 && m->mapped);   // host visible + coherent, cached preferred
        CHECK(a.cpu == cb->cpu);                     // block 0 = the start of the mapping
    }
    memset(a.cpu, 0x5a, (size_t)a.size);
    CHECK_EQ(used_of(VKN_ARENA_STATIC), 112); CHECK_EQ(res_of(VKN_ARENA_STATIC), 16u << 20);
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 1000, 256, &b), 0);
    CHECK_EQ(b.off, 256); CHECK_EQ(b.size, 1008); CHECK(b.buf == a.buf); CHECK(b.cpu == (uint8_t *)a.cpu + 256);
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 32, 16, &c), 0);
    CHECK_EQ(c.off, 112);                             // first fit: the alignment gap left by b
    // free b: merges with the gap before (144..256) and the tail after (1264..) -> one extent from 144 to the chunk end
    vkn_block_free(&b);
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, (16u << 20) - 144, 16, &d), 0);
    CHECK_EQ(d.off, 144); CHECK(d.buf == a.buf);
    CHECK_EQ(mk_created[MK_BUFFER], buf0 + 1);
    vkn_block_free(&d); vkn_block_free(&a); vkn_block_free(&c);
    CHECK_EQ(used_of(VKN_ARENA_STATIC), 0);
    // everything merged back: the whole chunk in one block, same chunk (16 MB chunks stay)
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 16u << 20, 16, &e), 0);
    CHECK_EQ(e.off, 0); CHECK(e.buf == a.buf); CHECK_EQ(mk_created[MK_BUFFER], buf0 + 1); CHECK_EQ(res_of(VKN_ARENA_STATIC), 16u << 20);
    // full chunk: the next block opens a second 16 MB chunk
    vkn_block f; CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 64, 0, &f), 0);
    CHECK(f.buf != e.buf); CHECK_EQ(f.off, 0); CHECK_EQ(res_of(VKN_ARENA_STATIC), 32u << 20); CHECK_EQ(mk_created[MK_BUFFER], buf0 + 2);
    vkn_block_free(&e); vkn_block_free(&f);
    CHECK_EQ(res_of(VKN_ARENA_STATIC), 32u << 20);    // 16 MB chunks are kept for reuse
    // reuse: a freed block's place is handed out again
    vkn_block g, h2;
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 64, 0, &g), 0); vkn_block_free(&g);
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 64, 0, &h2), 0); CHECK(h2.buf == g.buf && h2.off == g.off); vkn_block_free(&h2);
    // alignment
    vkn_block al[4]; const uint32_t aligns[4] = {16, 256, 4096, 65536};
    for (int i = 0; i < 4; i++) { CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 48, aligns[i], &al[i]), 0); CHECK_EQ(al[i].off % aligns[i], 0); CHECK_EQ(al[i].size, 48); }
    for (int i = 0; i < 4; i++) vkn_block_free(&al[i]);
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 48, 48, &g), -1);     // not a power of two
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 0, 16, &g), -1);
    CHECK_EQ(vkn_block_alloc(VKN_ARENAS, 16, 16, &g), -1);
    CHECK_EQ(vkn_block_alloc(-1, 16, 16, &g), -1);
    // classes never share a chunk
    vkn_block s1, st1; CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 64, 0, &st1), 0); CHECK_EQ(vkn_block_alloc(VKN_ARENA_STAGING, 64, 0, &s1), 0);
    CHECK(s1.buf != st1.buf); CHECK_EQ(s1.cls, VKN_ARENA_STAGING);
    vkn_block_free(&st1);
    // a dedicated chunk for a block above 16 MB: rounded to 64 KiB, dropped with its last block
    const uint64_t r0 = res_of(VKN_ARENA_STAGING), mem0 = mk_destroyed[MK_MEMORY], bufd0 = mk_destroyed[MK_BUFFER];
    vkn_block big; const uint64_t bs = (20u << 20) + 1;
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STAGING, bs, 0, &big), 0);
    CHECK_EQ(big.off, 0); CHECK_EQ(big.size, (bs + 15) & ~15ull); CHECK(big.buf != s1.buf);
    CHECK_EQ(res_of(VKN_ARENA_STAGING), r0 + ((bs + 0xffff) & ~0xffffull));
    mk_obj *bo = mk_obj_of((uint64_t)(uintptr_t)big.buf); CHECK(bo && bo->size == ((bs + 0xffff) & ~0xffffull));
    memset(big.cpu, 1, (size_t)big.size);              // the whole block is mapped
    vkn_block_free(&big);
    CHECK_EQ(res_of(VKN_ARENA_STAGING), r0);
    CHECK_EQ(mk_destroyed[MK_MEMORY], mem0 + 1); CHECK_EQ(mk_destroyed[MK_BUFFER], bufd0 + 1);
    CHECK(!mk_is_live((uint64_t)(uintptr_t)big.buf, MK_BUFFER));
    vkn_block_free(&s1);
    CHECK_EQ(used_of(VKN_ARENA_STAGING), 0); CHECK_EQ(used_of(VKN_ARENA_STATIC), 0);
    // many small blocks, freed in a scattered order, merge back into one
    enum { NB = 600 }; static vkn_block many[NB];
    for (int i = 0; i < NB; i++) CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 16 + 16 * (i % 7), 0, &many[i]), 0);
    for (int i = 0; i < NB; i += 2) vkn_block_free(&many[i]);
    for (int i = 1; i < NB; i += 2) vkn_block_free(&many[i]);
    CHECK_EQ(used_of(VKN_ARENA_STATIC), 0);
    CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 16u << 20, 16, &e), 0); CHECK_EQ(e.off, 0); vkn_block_free(&e);
}

static void test_retire_upload(void) {
    memset(g_ret_runs, 0, sizeof g_ret_runs);
    const uint64_t f0 = vkn_frame();
    vkn_retire(ret_fn, (void *)1);
    frames(1); vkn_retire(ret_fn, (void *)2);
    frames(1); CHECK_EQ(g_ret_runs[1], 0);
    frames(1); CHECK_EQ(g_ret_runs[1], 1); CHECK_EQ(g_ret_runs[2], 0);   // exactly 3 frame ends after the retire
    frames(1); CHECK_EQ(g_ret_runs[1], 1); CHECK_EQ(g_ret_runs[2], 1);
    CHECK_EQ(vkn_frame(), f0 + 4);
    vkn_retire(ret_fn, (void *)3); vkn_retire_all(); CHECK_EQ(g_ret_runs[3], 1);
    frames(5); CHECK_EQ(g_ret_runs[3], 1);
    // a retired block comes back after the delay
    vkn_block b; CHECK_EQ(vkn_block_alloc(VKN_ARENA_STAGING, 4096, 0, &b), 0);
    vkn_retire_block(&b);
    frames(2); CHECK_EQ(used_of(VKN_ARENA_STAGING), 4096);
    frames(1); CHECK_EQ(used_of(VKN_ARENA_STAGING), 0);
    // the upload queue: order, bytes, run
    g_up_n = 0;
    vkn_upload_push(up_fn, (void *)1, 100); vkn_upload_push(up_fn, (void *)2, 0); vkn_upload_push(up_fn, (void *)3, 28);
    CHECK_EQ(vkn_upload_pending_bytes(), 128);
    VkCommandBuffer tcb = mk_test_cb();
    CHECK_EQ(vkn_upload_run(tcb), 3);
    CHECK_EQ(g_up_n, 3); CHECK(g_up_order[0] == 1 && g_up_order[1] == 2 && g_up_order[2] == 3); CHECK(g_up_cb[0] == tcb && g_up_cb[2] == tcb);
    CHECK_EQ(vkn_upload_pending_bytes(), 0); CHECK_EQ(vkn_upload_run(tcb), 0);
    // flush: own command buffer, one submit under the queue lock, the fence waited for
    const int sub0 = mk_submits, ql0 = mk_qlocks; g_up_n = 0;
    CHECK_EQ(vkn_upload_flush(), 0); CHECK_EQ(mk_submits, sub0);   // nothing queued: nothing submitted
    vkn_upload_push(up_fn, (void *)7, 10); vkn_upload_push(up_fn, (void *)8, 10);
    CHECK_EQ(vkn_upload_flush(), 2);
    CHECK_EQ(mk_submits, sub0 + 1); CHECK_EQ(mk_qlocks, ql0 + 1); CHECK_EQ(mk_qlock_depth, 0);
    CHECK(g_up_n == 2 && g_up_order[0] == 7 && g_up_order[1] == 8); CHECK(g_up_cb[0] == mk_last_submitted && g_up_cb[0] != tcb);
    CHECK_EQ(mk_created[MK_POOL], 1); CHECK_EQ(mk_created[MK_FENCE], 1);
    vkn_upload_push(up_fn, (void *)9, 1); CHECK_EQ(vkn_upload_flush(), 1);
    CHECK_EQ(mk_created[MK_POOL], 1); CHECK_EQ(mk_submits, sub0 + 2); CHECK(g_up_cb[2] == g_up_cb[0]);   // the same one-shot buffer again
    // one-shot command buffers directly
    VkCommandBuffer ocb = vkn_once_begin(); CHECK(ocb == g_up_cb[0]); CHECK_EQ(vkn_once_end(ocb), 0); CHECK_EQ(mk_submits, sub0 + 3);
}

static void test_tab(void) {
    CHECK_EQ(vkn_h_idx(H(5, 0x3fffff)), 5); CHECK_EQ(vkn_h_gen(H(5, 0x3fffff)), 0x3fffff); CHECK_EQ(vkn_h_gen(0xffc0000100000007ull), 0x1);
    vkn_tab t; CHECK_EQ(vkn_tab_init(&t, 16, 8), 0);
    int alive = -1;
    uint64_t *r = (uint64_t *)vkn_tab_put(&t, H(3, 1), &alive);
    CHECK(r != NULL); CHECK_EQ(alive, 0); CHECK_EQ(*r, 0); *r = 0x1234;
    CHECK(vkn_tab_get(&t, H(3, 1)) == r); CHECK(vkn_tab_get(&t, H(3, 2)) == NULL); CHECK(vkn_tab_get(&t, H(4, 1)) == NULL);
    CHECK(vkn_tab_at(&t, 3) == r); CHECK(vkn_tab_at(&t, 16) == NULL);
    uint64_t *r2 = (uint64_t *)vkn_tab_put(&t, H(3, 2), &alive);   // a new generation of the same index
    CHECK(r2 == r); CHECK_EQ(alive, 1); CHECK_EQ(t.replaced, 1); CHECK_EQ(*r2, 0x1234);   // the old record, for the caller to reset
    CHECK(vkn_tab_get(&t, H(3, 1)) == NULL); CHECK(vkn_tab_get(&t, H(3, 2)) == r);
    CHECK_EQ(vkn_tab_kill(&t, H(3, 1)), 0); CHECK_EQ(vkn_tab_kill(&t, H(3, 2)), 1); CHECK_EQ(vkn_tab_kill(&t, H(3, 2)), 0);
    CHECK(vkn_tab_get(&t, H(3, 2)) == NULL); CHECK_EQ(t.kills, 1);
    CHECK(vkn_tab_put(&t, H(16, 1), NULL) == NULL); CHECK_EQ(t.overflow, 1);
    CHECK(vkn_tab_put(&t, H(15, 1), &alive) != NULL && alive == 0); CHECK(vkn_tab_get(&t, H(15, 1)) != NULL);
    CHECK(vkn_tab_get(&t, 0) == NULL);
    CHECK_EQ(t.puts, 3);
    free(t.rec); free(t.handle);
}

// ================================================================================================================ buffers
static void test_buffers(void) {
    const uint64_t u0 = used_of(VKN_ARENA_STATIC);
    uint8_t init[100]; for (int i = 0; i < 100; i++) init[i] = (uint8_t)(i * 7 + 1);
    vkn_block b1;
    CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(1, 1), 100, 0, 0, init, &b1), 0);
    CHECK(b1.size >= 100 && b1.size % 16 == 0); CHECK_EQ(b1.off % 256, 0); CHECK(!memcmp(b1.cpu, init, 100));
    vkn_buf_info bi; CHECK_EQ(vkn_buf_lookup(VKN_BUF_VB, H(1, 1), &bi), 0);
    CHECK_EQ(bi.size, 100); CHECK_EQ(bi.has_init, 1); CHECK_EQ(bi.kind, VKN_BUF_VB); CHECK_EQ(bi.usage, 0); CHECK(bi.blk.cpu == b1.cpu);
    CHECK(vkn_buf_get(VKN_BUF_VB, H(1, 1)) != NULL && vkn_buf_get(VKN_BUF_VB, H(1, 1))->off == b1.off);
    CHECK(vkn_buf_get(VKN_BUF_IB, H(1, 1)) == NULL);   // tables per kind
    CHECK(vkn_buf_get(VKN_BUF_VB, H(1, 2)) == NULL);   // another generation
    // without init data: zeroed even where an old block left bytes
    vkn_block dirty; CHECK_EQ(vkn_block_alloc(VKN_ARENA_STATIC, 256, 256, &dirty), 0); memset(dirty.cpu, 0xab, 256); vkn_block_free(&dirty);
    vkn_block b2; CHECK_EQ(vkn_buf_create(VKN_BUF_IB, H(2, 1), 64, 2, VKN_BUF_USAGE_DYNAMIC, NULL, &b2), 0);
    CHECK(b2.buf == dirty.buf && b2.off == dirty.off);
    int zero = 1; for (uint64_t i = 0; i < b2.size; i++) zero &= ((uint8_t *)b2.cpu)[i] == 0;
    CHECK(zero);
    CHECK_EQ(vkn_buf_lookup(VKN_BUF_IB, H(2, 1), &bi), 0); CHECK_EQ(bi.index_size, 2); CHECK_EQ(bi.has_init, 0); CHECK_EQ(bi.usage, VKN_BUF_USAGE_DYNAMIC);
    CHECK_EQ(vkn_buf_index_type(2), VK_INDEX_TYPE_UINT16); CHECK_EQ(vkn_buf_index_type(4), VK_INDEX_TYPE_UINT32); CHECK_EQ(vkn_buf_index_type(1), VK_INDEX_TYPE_UINT16);
    // size 0: still a block of its own
    vkn_block b0; CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(3, 1), 0, 0, 0, init, &b0), 0); CHECK_EQ(b0.size, 16); CHECK(b0.cpu != NULL && b0.off != b1.off);
    CHECK_EQ(vkn_buf_lookup(VKN_BUF_VB, H(3, 1), &bi), 0); CHECK_EQ(bi.has_init, 0);
    vkn_buf_stats st; vkn_buf_stats_get(&st);
    CHECK_EQ(st.created[VKN_BUF_VB], 2); CHECK_EQ(st.created[VKN_BUF_IB], 1); CHECK_EQ(st.live[VKN_BUF_VB], 2); CHECK_EQ(st.live_bytes[VKN_BUF_VB], 100);
    CHECK_EQ(st.bytes_static[VKN_BUF_VB], 100); CHECK_EQ(st.bytes_dynamic[VKN_BUF_IB], 64); CHECK_EQ(st.with_init[VKN_BUF_VB], 1); CHECK_EQ(st.without_init[VKN_BUF_VB], 1);
    // pseudo addresses
    const uint64_t va = vkn_buf_va(&b1);
    CHECK_EQ(va >> 48, 0xb0);
    VkBuffer rb = VK_NULL_HANDLE; uint64_t ro = 0;
    CHECK_EQ(vkn_buf_resolve(va + 40, &rb, &ro), 0); CHECK(rb == b1.buf); CHECK_EQ(ro, b1.off + 40);
    CHECK_EQ(vkn_buf_resolve(0x12345678ull, &rb, &ro), -1); CHECK_EQ(vkn_buf_resolve(VKN_BUF_VA_TAG | 4000ull << 36, &rb, &ro), -1);
    // copy-in
    const uint8_t pat[8] = {9, 8, 7, 6, 5, 4, 3, 2};
    CHECK_EQ(vkn_buf_copy_in(VKN_BUF_VB, H(1, 1), 50, pat, 8), 0); CHECK(!memcmp((uint8_t *)b1.cpu + 50, pat, 8));
    CHECK_EQ(vkn_buf_copy_in(VKN_BUF_VB, H(1, 1), b1.size - 4, pat, 8), -2); CHECK_EQ(vkn_buf_copy_in(VKN_BUF_VB, H(1, 1), b1.size + 1, pat, 0), -2);
    CHECK_EQ(vkn_buf_copy_in(VKN_BUF_VB, H(9, 1), 0, pat, 8), -1);
    // destroy: the block is retired, comes back to the arena only after 3 frame ends
    const uint64_t u1 = used_of(VKN_ARENA_STATIC);
    CHECK_EQ(u1, u0 + b1.size + b2.size + b0.size);
    vkn_buf_destroy(VKN_BUF_VB, H(1, 1));
    CHECK(vkn_buf_get(VKN_BUF_VB, H(1, 1)) == NULL); CHECK_EQ(vkn_buf_lookup(VKN_BUF_VB, H(1, 1), &bi), -1);
    frames(2); CHECK_EQ(used_of(VKN_ARENA_STATIC), u1);
    frames(1); CHECK_EQ(used_of(VKN_ARENA_STATIC), u1 - b1.size);
    vkn_buf_destroy(VKN_BUF_VB, H(1, 1)); vkn_buf_destroy(VKN_BUF_VB, H(77, 1));
    vkn_buf_stats_get(&st); CHECK_EQ(st.destroyed[VKN_BUF_VB], 1); CHECK_EQ(st.unknown_destroy[VKN_BUF_VB], 2); CHECK_EQ(st.live[VKN_BUF_VB], 1);
    // a create over a live index (another generation): the old record is replaced, its block retired
    const uint64_t u2 = used_of(VKN_ARENA_STATIC);
    vkn_block b3; CHECK_EQ(vkn_buf_create(VKN_BUF_IB, H(2, 2), 32, 4, 0, NULL, &b3), 0);
    CHECK(vkn_buf_get(VKN_BUF_IB, H(2, 1)) == NULL); CHECK(vkn_buf_get(VKN_BUF_IB, H(2, 2)) != NULL);
    vkn_buf_stats_get(&st); CHECK_EQ(st.replaced[VKN_BUF_IB], 1); CHECK_EQ(st.live[VKN_BUF_IB], 1); CHECK_EQ(st.live_bytes[VKN_BUF_IB], 32);
    frames(3); CHECK_EQ(used_of(VKN_ARENA_STATIC), u2 - b2.size + b3.size);
    // index out of the table
    CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(VKN_BUF_TAB, 1), 16, 0, 0, NULL, NULL), -1);
    CHECK_EQ(vkn_buf_create(2, H(4, 1), 16, 0, 0, NULL, NULL), -1);
    // noted only (mode 2): bookkeeping without memory
    CHECK_EQ(vkn_buf_note(VKN_BUF_VB, H(10, 1), 4096, 0, 3, 1), 0);
    CHECK_EQ(vkn_buf_lookup(VKN_BUF_VB, H(10, 1), &bi), 0); CHECK_EQ(bi.blk.size, 0); CHECK_EQ(bi.size, 4096); CHECK(vkn_buf_get(VKN_BUF_VB, H(10, 1)) == NULL);
    CHECK_EQ(vkn_buf_copy_in(VKN_BUF_VB, H(10, 1), 0, pat, 8), -1);
    const uint64_t u3 = used_of(VKN_ARENA_STATIC);
    vkn_buf_destroy(VKN_BUF_VB, H(10, 1)); frames(3); CHECK_EQ(used_of(VKN_ARENA_STATIC), u3);
    vkn_buf_destroy(VKN_BUF_VB, H(3, 1)); vkn_buf_destroy(VKN_BUF_IB, H(2, 2)); frames(3);
    CHECK_EQ(used_of(VKN_ARENA_STATIC), u0);
}

static void test_ring(void) {
    const uint32_t R = VKN_RING_BYTES;
    CHECK_EQ(R, 0x1380000u);
    CHECK(vkn_ring_buffer() == VK_NULL_HANDLE);   // not made before its first use
    uint32_t off = 77;
    CHECK_EQ(vkn_ring_alloc(100, &off), 0); CHECK_EQ(off, 0);
    CHECK(vkn_ring_buffer() != VK_NULL_HANDLE); CHECK(vkn_ring_cpu() != NULL); CHECK_EQ(vkn_ring_base(), 0);
    CHECK_EQ(res_of(VKN_ARENA_RING), R);          // a dedicated chunk (bigger than 16 MB, a multiple of 64 KiB already)
    mk_obj *rb = mk_obj_of((uint64_t)(uintptr_t)vkn_ring_buffer()); CHECK(rb && rb->size == R);
    memset((uint8_t *)vkn_ring_cpu() + R - 16, 0xcd, 16);   // the whole ring is mapped
    CHECK_EQ(vkn_ring_alloc(10, &off), 0); CHECK_EQ(off, 256);    // head 100 -> 256
    CHECK_EQ(vkn_ring_alloc(256, &off), 0); CHECK_EQ(off, 512);   // head 266 -> 512
    CHECK_EQ(vkn_ring_alloc(1, &off), 0); CHECK_EQ(off, 768);     // head 768 is aligned already
    // head 769 -> 1024; 1024 + (R - 1024 - 1) = R - 1 < R: no wrap
    CHECK_EQ(vkn_ring_alloc(R - 1024 - 1, &off), 0); CHECK_EQ(off, 1024);
    // head R - 1 -> aligned up to R; R + 1 >= R: wrap
    CHECK_EQ(vkn_ring_alloc(1, &off), 0); CHECK_EQ(off, 0);
    vkn_buf_stats st; vkn_buf_stats_get(&st); CHECK_EQ(st.ring_wraps, 1);
    // the boundary itself: head 256, size R - 256 -> head + size == R exactly: wraps (">=" of 0x925740)
    CHECK_EQ(vkn_ring_alloc(255, &off), 0); CHECK_EQ(off, 256);   // head 1 -> 256; head = 511
    CHECK_EQ(vkn_ring_alloc(R - 512, &off), 0); CHECK_EQ(off, 0);  // head 511 -> 512; 512 + R - 512 == R -> wrap to 0; head = R - 512
    vkn_buf_stats_get(&st); CHECK_EQ(st.ring_wraps, 2);
    CHECK_EQ(vkn_ring_alloc(511, &off), 0); CHECK_EQ(off, R - 512); // head R - 512 aligned; R - 512 + 511 = R - 1 < R: no wrap
    vkn_buf_stats_get(&st); CHECK_EQ(st.ring_wraps, 2);
    CHECK_EQ(vkn_ring_alloc(R, &off), -1); CHECK_EQ(vkn_ring_alloc(R + 5, &off), -1);
    CHECK_EQ(vkn_ring_alloc(R - 1, &off), 0); CHECK_EQ(off, 0);    // (head R - 1 -> R) + R - 1 >= R: wrap; 0 + R - 1 fits
    vkn_buf_stats_get(&st); CHECK_EQ(st.ring_fail, 2); CHECK_EQ(st.ring_allocs, 10); CHECK_EQ(st.ring_wraps, 3);
}

// ================================================================================================================ stage 4: copies
// vkn_buf_update_ex: in place while no recording of the last 3 frames read the block; renamed (old bytes carried, only inside the
// engine size) once a draw path got it; whole-buffer updates carry nothing; has_block does not mark the buffer used.
static void test_buf_update(void) {
    const uint64_t s0 = used_of(VKN_ARENA_STAGING), u0 = used_of(VKN_ARENA_STATIC);
    uint8_t init[1000], pat[1000];
    for (int i = 0; i < 1000; i++) { init[i] = (uint8_t)i; pat[i] = (uint8_t)(0xa0 ^ i); }
    vkn_block b; CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(20, 1), 1000, 0, VKN_BUF_USAGE_DYNAMIC, init, &b), 0);
    CHECK(b.size >= 1000);
    vkn_buf_ustats u0s; vkn_buf_ustats_get(&u0s);
    void *cpu = NULL; uint64_t va = 0; vkn_buf_uinfo in;
    // never handed to a draw: in place
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(20, 1), 100, pat, 50, &cpu, &va, &in), 0);
    CHECK(cpu == NULL); CHECK_EQ(in.renamed, 0); CHECK_EQ(in.whole, 0); CHECK_EQ(in.usage, VKN_BUF_USAGE_DYNAMIC);
    CHECK(!memcmp((uint8_t *)b.cpu + 100, pat, 50)); CHECK(!memcmp(b.cpu, init, 100)); CHECK(!memcmp((uint8_t *)b.cpu + 150, init + 150, 850));
    CHECK_EQ(vkn_buf_has_block(VKN_BUF_VB, H(20, 1)), 1); CHECK_EQ(vkn_buf_has_block(VKN_BUF_VB, H(20, 2)), 0); CHECK_EQ(vkn_buf_has_block(VKN_BUF_IB, H(20, 1)), 0);
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(20, 1), 0, pat, 4, &cpu, &va, &in), 0);   // has_block did not mark it used: still in place
    memcpy(b.cpu, init, 4);
    // a draw path got it this frame: renamed, everything outside the update carried over
    CHECK(vkn_buf_get(VKN_BUF_VB, H(20, 1)) != NULL);
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(20, 1), 200, pat, 100, &cpu, &va, &in), 1);
    CHECK(cpu != NULL && cpu != b.cpu); CHECK_EQ(in.renamed, 1); CHECK_EQ(in.carried, 900); CHECK_EQ(in.whole, 0);
    const vkn_block *nb = vkn_buf_get(VKN_BUF_VB, H(20, 1));
    CHECK(nb && nb->cpu == cpu && va == vkn_buf_va(nb));
    CHECK(!memcmp(cpu, init, 100)); CHECK(!memcmp((uint8_t *)cpu + 100, pat, 50)); CHECK(!memcmp((uint8_t *)cpu + 150, init + 150, 50));
    CHECK(!memcmp((uint8_t *)cpu + 200, pat, 100)); CHECK(!memcmp((uint8_t *)cpu + 300, init + 300, 700));
    CHECK(!memcmp((uint8_t *)b.cpu + 200, init + 200, 100));   // the old block (draws recorded before) kept its bytes
    // used again (the vkn_buf_get above): a whole-buffer update renames without carrying (the block's rounding is not the engine's)
    void *cpu2 = NULL;
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(20, 1), 0, pat, 1000, &cpu2, &va, &in), 1);
    CHECK(cpu2 != NULL && cpu2 != cpu); CHECK_EQ(in.carried, 0); CHECK_EQ(in.whole, 1); CHECK(!memcmp(cpu2, pat, 1000));
    vkn_buf_ustats us; vkn_buf_ustats_get(&us);
    CHECK_EQ(us.inplace - u0s.inplace, 2); CHECK_EQ(us.renamed - u0s.renamed, 2); CHECK_EQ(us.renamed_dyn - u0s.renamed_dyn, 2);
    CHECK_EQ(us.rest_dyn - u0s.rest_dyn, 900); CHECK_EQ(us.rest_bytes - u0s.rest_bytes, 900); CHECK_EQ(us.whole - u0s.whole, 1); CHECK_EQ(us.partial - u0s.partial, 3);
    CHECK_EQ(us.bytes - u0s.bytes, 50 + 4 + 100 + 1000);
    // 3 frames after the last use: in place again
    frames(3);
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(20, 1), 10, pat, 10, &cpu, &va, &in), 0); CHECK(!memcmp((uint8_t *)cpu2 + 10, pat, 10));
    // errors
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(20, 1), 995, pat, 100, NULL, NULL, &in), -2);
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(20, 1), 0, NULL, 4, NULL, NULL, &in), -2);
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(99, 1), 0, pat, 4, NULL, NULL, &in), -1);
    CHECK_EQ(vkn_buf_update_ex(5, H(20, 1), 0, pat, 4, NULL, NULL, &in), -1);
    CHECK_EQ(vkn_buf_update(VKN_BUF_VB, H(20, 1), 0, pat, 4, NULL, NULL), 0);   // the old entry is the same function
    // a static IB: renames counted as static; a size-0 buffer (block of 16) carries nothing past the update
    vkn_block bi; CHECK_EQ(vkn_buf_create(VKN_BUF_IB, H(21, 1), 64, 2, 0, init, &bi), 0);
    CHECK(vkn_buf_get(VKN_BUF_IB, H(21, 1)) != NULL);
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_IB, H(21, 1), 32, pat, 32, NULL, NULL, &in), 1); CHECK_EQ(in.carried, 32); CHECK_EQ(in.usage, 0);
    vkn_block bz; CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(22, 1), 0, 0, VKN_BUF_USAGE_DYNAMIC, NULL, &bz), 0);
    CHECK(vkn_buf_get(VKN_BUF_VB, H(22, 1)) != NULL);
    CHECK_EQ(vkn_buf_update_ex(VKN_BUF_VB, H(22, 1), 0, pat, 8, NULL, NULL, &in), 1); CHECK_EQ(in.carried, 0); CHECK_EQ(in.whole, 1);
    vkn_buf_ustats_get(&us); CHECK_EQ(us.renamed_static - u0s.renamed_static, 1); CHECK_EQ(us.rest_static - u0s.rest_static, 32);
    // destroys: every version comes back after the retire delay
    vkn_buf_destroy(VKN_BUF_VB, H(20, 1)); vkn_buf_destroy(VKN_BUF_IB, H(21, 1)); vkn_buf_destroy(VKN_BUF_VB, H(22, 1));
    frames(3);
    CHECK_EQ(used_of(VKN_ARENA_STAGING), s0); CHECK_EQ(used_of(VKN_ARENA_STATIC), u0);
}

// vkn_mirror: changed 16-row bands of a CPU-written region, runs, limits, forgetting, eviction.
static void test_mirror(void) {
    enum { RB = 100, ST = 128, ROWS = 96 };
    static uint8_t m[ST * ROWS];
    for (int i = 0; i < ST * ROWS; i++) m[i] = (uint8_t)(i * 7);
    // the hash: deterministic, sensitive to every byte and to the length, seeds differ
    CHECK(vkn_mirror_hash(m, 37, 0) == vkn_mirror_hash(m, 37, 0)); CHECK(vkn_mirror_hash(m, 37, 0) != vkn_mirror_hash(m, 36, 0));
    CHECK(vkn_mirror_hash(m, 37, 0) != vkn_mirror_hash(m, 37, 1));
    { uint8_t z[64]; memset(z, 0, sizeof z); const uint64_t h0 = vkn_mirror_hash(z, 64, 0); z[63] = 1; CHECK(vkn_mirror_hash(z, 64, 0) != h0); z[63] = 0; z[0] = 0x80; CHECK(vkn_mirror_hash(z, 64, 0) != h0); }
    vkn_mirror_stats a; vkn_mirror_stats_get(&a);
    vkn_mirror_run r[VKN_MIRROR_RUNS]; int full = 0;
    CHECK_EQ(vkn_mirror_diff(1, 7, m, ST, RB, ROWS, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(full, 1); CHECK_EQ(r[0].y, 0); CHECK_EQ(r[0].rows, ROWS);
    CHECK_EQ(vkn_mirror_diff(1, 7, m, ST, RB, ROWS, r, VKN_MIRROR_RUNS, &full), 0); CHECK_EQ(full, 0);   // nothing changed
    m[20 * ST + 99] ^= 1;   // row 20, last byte of the row: band 1
    CHECK_EQ(vkn_mirror_diff(1, 7, m, ST, RB, ROWS, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(r[0].y, 16); CHECK_EQ(r[0].rows, 16);
    m[20 * ST + 100] ^= 1;  // the stride padding is not part of the region
    CHECK_EQ(vkn_mirror_diff(1, 7, m, ST, RB, ROWS, r, VKN_MIRROR_RUNS, &full), 0);
    m[0] ^= 1; m[95 * ST] ^= 1;   // bands 0 and 5
    CHECK_EQ(vkn_mirror_diff(1, 7, m, ST, RB, ROWS, r, VKN_MIRROR_RUNS, &full), 2);
    CHECK_EQ(r[0].y, 0); CHECK_EQ(r[0].rows, 16); CHECK_EQ(r[1].y, 80); CHECK_EQ(r[1].rows, 16);
    m[17 * ST] ^= 1; m[40 * ST + 5] ^= 1;   // bands 1 and 2: adjacent, one run
    CHECK_EQ(vkn_mirror_diff(1, 7, m, ST, RB, ROWS, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(r[0].y, 16); CHECK_EQ(r[0].rows, 32);
    m[1] ^= 1; m[33 * ST] ^= 1; m[70 * ST] ^= 1;   // bands 0, 2, 4 with room for 2 runs: the last run grows over band 3
    CHECK_EQ(vkn_mirror_diff(1, 7, m, ST, RB, ROWS, r, 2, &full), 2);
    CHECK_EQ(r[0].y, 0); CHECK_EQ(r[0].rows, 16); CHECK_EQ(r[1].y, 32); CHECK_EQ(r[1].rows, 48);
    // another generation / geometry / a forget: full again
    CHECK_EQ(vkn_mirror_diff(1, 8, m, ST, RB, ROWS, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(full, 1);
    CHECK_EQ(vkn_mirror_diff(1, 8, m, ST, RB, ROWS - 8, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(full, 1); CHECK_EQ(r[0].rows, ROWS - 8);
    m[87 * ST] ^= 1;   // the short last band (rows 80..87)
    CHECK_EQ(vkn_mirror_diff(1, 8, m, ST, RB, ROWS - 8, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(r[0].y, 80); CHECK_EQ(r[0].rows, 8);
    vkn_mirror_forget(1);
    CHECK_EQ(vkn_mirror_diff(1, 8, m, ST, RB, ROWS - 8, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(full, 1);
    // bad arguments
    CHECK_EQ(vkn_mirror_diff(2, 0, NULL, ST, RB, ROWS, r, 4, &full), -1); CHECK_EQ(vkn_mirror_diff(2, 0, m, 64, RB, ROWS, r, 4, &full), -1);
    CHECK_EQ(vkn_mirror_diff(2, 0, m, ST, RB, 0, r, 4, &full), -1); CHECK_EQ(vkn_mirror_diff(2, 0, m, ST, RB, ROWS, r, 0, &full), -1);
    // more keys than the table: the least recently used one is evicted (key 1 was used last before the loop, so it goes first)
    for (uint64_t k = 100; k < 100 + VKN_MIRROR_KEYS; k++) CHECK_EQ(vkn_mirror_diff(k, 0, m, ST, RB, 16, r, 4, &full), 1);
    CHECK_EQ(vkn_mirror_diff(1, 8, m, ST, RB, ROWS - 8, r, VKN_MIRROR_RUNS, &full), 1); CHECK_EQ(full, 1);
    CHECK_EQ(vkn_mirror_diff(100 + VKN_MIRROR_KEYS - 1, 0, m, ST, RB, 16, r, 4, &full), 0);   // a recent one is still known
    vkn_mirror_stats b2; vkn_mirror_stats_get(&b2);
    CHECK_EQ(b2.merged - a.merged, 1); CHECK(b2.evicted - a.evicted >= 2); CHECK_EQ(b2.forgot - a.forgot, 1);
    vkn_mirror_forget_all();
    CHECK_EQ(vkn_mirror_diff(100 + VKN_MIRROR_KEYS - 1, 0, m, ST, RB, 16, r, 4, &full), 1); CHECK_EQ(full, 1);
    vkn_mirror_forget_all();
}

// ================================================================================================================ main
static void test_textures(void);
static void test_state(void);
static void test_shaders(void);
static void test_support_drs(void);
static void test_leaks(void);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    mk_main_thread = pthread_self();
    vkn_set_mode(VKN_ALONGSIDE); CHECK_EQ(vkn_mode(), VKN_ALONGSIDE); vkn_set_mode(7); CHECK_EQ(vkn_mode(), VKN_OFF); vkn_set_mode(VKN_NATIVE); CHECK_EQ(vkn_mode(), VKN_NATIVE);
    test_no_device();
    test_arena();
    test_retire_upload();
    test_tab();
    test_buffers();
    test_ring();
    test_buf_update();
    test_mirror();
    test_textures();
    test_state();
    test_shaders();
    test_support_drs();
    test_leaks();
    CHECK_EQ(mk_errors, 0);
    if (g_fails) { printf("vkn_stage2_test: %d of %d checks FAILED\n", g_fails, g_checks); return 1; }
    printf("vkn_stage2_test: %d checks passed\n", g_checks);
    return 0;
}

// ================================================================================================================ textures
// NVN format number -> VkFormat, written here from the NVN format enum (nvn_Types.h order: R8 .. RGBA32I, stencil / depth, RGBX8_SRGB,
// RGBA8_SRGB, packed, DXT / RGTC / BPTC, BGR(A/X)8): independent of vkn_tex.c's table. x = 1: alpha reads as one (an RGBX format).
static VkFormat exp_nvn_vk(uint32_t n, int *x) {
    static const VkFormat lo[0x37] = {VK_FORMAT_UNDEFINED,
        /*01*/ VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
        /*05*/ VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
        /*0a*/ VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
        /*0d*/ VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        /*11*/ VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
        /*16*/ VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        /*19*/ VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT,
        /*1d*/ VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT,
        /*22*/ VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
        /*25*/ VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        /*29*/ VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT,
        /*2e*/ VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        /*31*/ VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    *x = 0;
    if (n < 0x37) return lo[n];
    switch (n) {
    case 0x37: *x = 1; return VK_FORMAT_R8G8B8A8_SRGB;           // RGBX8_SRGB
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x3d: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;        // RGB10A2: R in the low bits
    case 0x3e: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;         // R11G11B10F
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;          // RGB9E5F
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;   case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;       case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;    case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;        case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;       case 0x4a: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4b: return VK_FORMAT_BC5_UNORM_BLOCK;       case 0x4c: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4d: return VK_FORMAT_BC7_UNORM_BLOCK;       case 0x4e: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4f: return VK_FORMAT_BC6H_SFLOAT_BLOCK;     case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x6d: *x = 1; return VK_FORMAT_R8G8B8A8_UNORM;          // RGBX8
    case 0x99: *x = 1; return VK_FORMAT_B8G8R8A8_UNORM;          // BGRX8
    case 0x9a: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x9b: *x = 1; return VK_FORMAT_B8G8R8A8_SRGB;
    case 0x9c: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static int packed16(VkFormat f) {   // RGBA4 / RGB565 / A1RGB5: bit orders are the code's own guesses; only "some 16-bit packed format" is checked
    return f == VK_FORMAT_R4G4B4A4_UNORM_PACK16 || f == VK_FORMAT_B4G4R4A4_UNORM_PACK16 || f == VK_FORMAT_R5G6B5_UNORM_PACK16 || f == VK_FORMAT_B5G6R5_UNORM_PACK16 ||
           f == VK_FORMAT_R5G5B5A1_UNORM_PACK16 || f == VK_FORMAT_B5G5R5A1_UNORM_PACK16 || f == VK_FORMAT_A1R5G5B5_UNORM_PACK16;
}
// The 3-component formats widened to 4 components (alpha one) when the device does not sample them.
static VkFormat widened_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM; case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT; case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT; case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM; case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT; case VK_FORMAT_R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT: return VK_FORMAT_R32G32B32A32_UINT; case VK_FORMAT_R32G32B32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    default: return f;
    }
}
static vkn_tex_desc tdesc(uint32_t efmt, uint32_t tg, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, uint32_t cls, uint64_t cookie, uint32_t pool_id) {
    vkn_tex_desc d; memset(&d, 0, sizeof d);
    d.efmt = efmt; d.nvn_target = tg; d.w = w; d.h = h; d.depth = 1; d.mips = mips; d.layers = layers; d.samples = 1; d.cls = cls;
    d.pool_id = pool_id; for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) d.layer_ids[i] = ~0u;
    d.sampler_id = ~0u; d.cookie = cookie;
    return d;
}
static const mk_copy_t *copy_of(VkImage img, uint32_t level, uint32_t layer, uint32_t z) {   // the logged copy of a subresource
    for (uint32_t i = 0; i < mk_ncopy; i++)
        if (mk_copy[i].img == img && mk_copy[i].c.imageSubresource.mipLevel == level && mk_copy[i].c.imageSubresource.baseArrayLayer == layer && (uint32_t)mk_copy[i].c.imageOffset.z == z) return &mk_copy[i];
    return NULL;
}
static uint32_t copies_of(VkImage img) { uint32_t n = 0; for (uint32_t i = 0; i < mk_ncopy; i++) n += mk_copy[i].img == img; return n; }
static uint32_t bars_of(VkImage img) { uint32_t n = 0; for (uint32_t i = 0; i < mk_nbar; i++) n += mk_bar[i].img == img; return n; }
static const mk_bar_t *bar_of(VkImage img, int k) { for (uint32_t i = 0; i < mk_nbar; i++) if (mk_bar[i].img == img && !k--) return &mk_bar[i]; return NULL; }
static uint32_t gob_off(uint32_t x, uint32_t y) {   // the Tegra X1 GOB (64 bytes x 8 rows): byte x of row y
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}
static uint64_t bl_off(uint32_t x, uint32_t y, uint32_t row_bytes, unsigned th) {   // block-linear, one slice, block height 2^th GOBs
    const uint32_t bh = 8u << th, gobs_x = (row_bytes + 63) / 64;
    return (uint64_t)(y / bh) * gobs_x * (512u << th) + (x / 64) * (512u << th) + ((y % bh) / 8) * 512 + gob_off(x, y);
}
static void bc1_solid(uint8_t *b, uint16_t c0) { memset(b, 0, 8); b[0] = (uint8_t)c0; b[1] = (uint8_t)(c0 >> 8); }   // c0 > c1 = 0, indices 0
static int all_rgba(const uint8_t *p, uint32_t n, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (uint32_t i = 0; i < n; i++) if (p[4 * i] != r || p[4 * i + 1] != g || p[4 * i + 2] != b || p[4 * i + 3] != a) return 0;
    return 1;
}
static mk_obj *img_of(const vkn_tex *t) { return t ? mk_obj_of((uint64_t)(uintptr_t)t->image) : NULL; }
static mk_obj *view_of(VkImageView v) { return v ? mk_obj_of((uint64_t)(uintptr_t)v) : NULL; }
static void flush_logged(void) { mk_log_reset(); vkn_upload_flush(); }

static void test_tex_formats(void) {
    // the engine format table: 0x919c20 as nr_formats has it (checked against main.raw), NVN -> Vulkan by the enum
    for (uint32_t e = 0; e < 63; e++)
        for (int s = 0; s < 2; s++) {
            uint32_t nvn = 0xdead; const VkFormat f = vkn_tex_engine_format(e, s, &nvn);
            const uint32_t want_nvn = s ? nr_formats[e].nvn_srgb : nr_formats[e].nvn;
            CHECK_EQ(nvn, want_nvn == NR_NVN_TRAP ? 0 : want_nvn);
            int x; const VkFormat want = want_nvn == NR_NVN_TRAP ? VK_FORMAT_UNDEFINED : exp_nvn_vk(want_nvn, &x);
            if (want_nvn == 0x39 || want_nvn == 0x95 || want_nvn == 0x98) CHECK(packed16(f));
            else CHECK_EQ(f, want);
            CHECK((f == VK_FORMAT_UNDEFINED) == (e == 0 || e == 41 || e == 47 || e == 62));   // None, RGBE8 (no NVN format), the two traps
        }
    // sRGB pairs (the sRGB bit of CreateTexture / CreateTextureFromImage, MAP-1 §5: 0x25 <-> 0x38, 0x41..0x44 <-> 0x45..0x48, 0x6d <-> 0x37)
    CHECK_EQ(vkn_tex_engine_format(4, 0, NULL), VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(vkn_tex_engine_format(4, 1, NULL), VK_FORMAT_R8G8B8A8_SRGB);
    CHECK_EQ(vkn_tex_engine_format(53, 1, NULL), VK_FORMAT_BC1_RGB_SRGB_BLOCK); CHECK_EQ(vkn_tex_engine_format(54, 1, NULL), VK_FORMAT_BC2_SRGB_BLOCK);
    CHECK_EQ(vkn_tex_engine_format(55, 1, NULL), VK_FORMAT_BC3_SRGB_BLOCK); CHECK_EQ(vkn_tex_engine_format(61, 1, NULL), VK_FORMAT_B8G8R8A8_SRGB);
    CHECK_EQ(vkn_tex_engine_format(59, 1, NULL), VK_FORMAT_BC7_UNORM_BLOCK);   // the engine keeps BC7 linear (nr_formats: 0x4d both)
    for (uint32_t n = 0x41; n <= 0x44; n++) { int x; CHECK_EQ(vkn_tex_nvn_format(n + 4), exp_nvn_vk(n + 4, &x)); }
    CHECK_EQ(vkn_tex_nvn_format(0x6d), VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(vkn_tex_nvn_format(0x37), VK_FORMAT_R8G8B8A8_SRGB);
    for (uint32_t n = 1; n < 0x9d; n++) {   // every NVN number either maps as the enum says or not at all (ASTC aside)
        int x; const VkFormat want = exp_nvn_vk(n, &x), got = vkn_tex_nvn_format(n);
        if (n >= 0x79 && n <= 0x94) { CHECK(got >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && got <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK); continue; }
        if (n == 0x39 || n == 0x3c || n == 0x95 || n == 0x98) { CHECK(packed16(got)); continue; }
        if (want != VK_FORMAT_UNDEFINED) CHECK_EQ(got, want);
    }
    CHECK_EQ(vkn_tex_nvn_format(0x79), VK_FORMAT_ASTC_4x4_UNORM_BLOCK); CHECK_EQ(vkn_tex_nvn_format(0x87), VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
    CHECK_EQ(vkn_tex_nvn_format(0x86), VK_FORMAT_ASTC_12x12_UNORM_BLOCK); CHECK_EQ(vkn_tex_nvn_format(0x94), VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    // every engine format the engine can create makes a sampled texture on this device (3-component formats widened), or is reported
    mk_bc = 1; mk_rgb3 = 0;
    for (uint32_t e = 0; e < 63; e++) {
        vkn_tex_desc d = tdesc(e, 1, 8, 8, 1, 1, VKN_TC_SAMPLED, 0x100000 + e, ~0u);
        const char *why = "x"; vkn_tex *t = vkn_tex_create(H(1000 + e, 1), &d, NULL, &why);
        if (e == 0 || e == 41 || e == 47 || e == 62) { CHECK(t == NULL); CHECK(why && !strcmp(why, "format without a Vulkan equivalent")); continue; }
        CHECK(t != NULL); if (!t) { printf("  engine format %u: %s\n", e, why); continue; }
        CHECK(why == NULL);
        int x; const VkFormat base = exp_nvn_vk(nr_formats[e].nvn, &x);
        if (nr_formats[e].nvn == 0x39 || nr_formats[e].nvn == 0x95 || nr_formats[e].nvn == 0x98) CHECK(packed16(t->format));
        else CHECK_EQ(t->format, widened_of(base));
        CHECK_EQ(t->widened, widened_of(base) != base);
        mk_obj *im = img_of(t); CHECK(im && im->ici.format == t->format);
        mk_obj *v = view_of(t->view); CHECK(v && v->vci.format == t->format);
        if (v) CHECK_EQ(v->vci.components.a, x ? VK_COMPONENT_SWIZZLE_ONE : (nr_formats[e].nvn == 0x39 ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_IDENTITY));   // BGR8 = BGRX8
        if (v && e >= 49 && e <= 52) CHECK_EQ(v->vci.subresourceRange.aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT);   // depth formats sample depth
        CHECK_EQ(vkn_tex_destroy_cookie(0x100000 + e), 1);
    }
    flush_logged(); frames(3);
    // BC formats on a device without BC sampling: decoded on the CPU, to these formats
    mk_bc = 0;
    static const struct { uint32_t e; int srgb; VkFormat f; } bc[] = {
        {53, 0, VK_FORMAT_R8G8B8A8_UNORM}, {54, 0, VK_FORMAT_R8G8B8A8_UNORM}, {55, 0, VK_FORMAT_R8G8B8A8_UNORM}, {59, 0, VK_FORMAT_R8G8B8A8_UNORM},
        {53, 1, VK_FORMAT_R8G8B8A8_SRGB}, {54, 1, VK_FORMAT_R8G8B8A8_SRGB}, {55, 1, VK_FORMAT_R8G8B8A8_SRGB},
        {56, 0, VK_FORMAT_R8_UNORM}, {57, 0, VK_FORMAT_R8G8_UNORM}, {58, 0, VK_FORMAT_R16G16B16A16_SFLOAT}};
    for (unsigned i = 0; i < sizeof bc / sizeof *bc; i++) {
        vkn_tex_desc d = tdesc(bc[i].e, 1, 8, 8, 1, 1, VKN_TC_SAMPLED, 0x110000 + i, ~0u); d.srgb = (uint8_t)bc[i].srgb;
        vkn_tex *t = vkn_tex_create(H(1100 + i, 1), &d, NULL, NULL);
        CHECK(t != NULL); if (!t) continue;
        CHECK_EQ(t->format, bc[i].f); CHECK_EQ(t->decoded, 1);
        CHECK_EQ(vkn_tex_destroy_cookie(0x110000 + i), 1);
    }
    mk_bc = 1;
    flush_logged(); frames(3);
}

static void test_tex_linear(void) {
    mk_bc = 1; mk_rgb3 = 0;
    // a 2D texture, 2 levels of linear RGBA8 data; two regions outside the image are skipped
    uint8_t l0[8 * 4 * 4], l1[4 * 2 * 4], e0[sizeof l0], e1[sizeof l1];
    for (unsigned i = 0; i < sizeof l0; i++) l0[i] = e0[i] = (uint8_t)(i * 3 + 1);
    for (unsigned i = 0; i < sizeof l1; i++) l1[i] = e1[i] = (uint8_t)(200 - i);
    vkn_tex_region rg[4] = {{0, 0, 0, 8, 4, 1, l0}, {5, 0, 0, 1, 1, 1, l0}, {1, 0, 0, 4, 2, 1, l1}, {0, 1, 0, 8, 4, 1, l0}};
    vkn_tex_src src = {VKN_SRC_REGIONS, rg, 4, NULL, 0, 0, 0};
    vkn_tex_desc d = tdesc(4, 1, 8, 4, 2, 1, VKN_TC_SAMPLED, 0xC2D, 100);
    const uint64_t pend0 = vkn_upload_pending_bytes(), st0 = used_of(VKN_ARENA_STAGING);
    vkn_tex *t = vkn_tex_create(H(5, 1), &d, &src, NULL);
    CHECK(t != NULL); if (!t) return;
    memset(l0, 0, sizeof l0); memset(l1, 0, sizeof l1);   // the data was copied at creation
    CHECK_EQ(vkn_upload_pending_bytes(), pend0 + 160);    // 128, aligned to 16, + 32
    CHECK_EQ(used_of(VKN_ARENA_STAGING), st0 + 160);
    mk_obj *im = img_of(t); CHECK(im != NULL); if (!im) return;
    CHECK_EQ(im->ici.imageType, VK_IMAGE_TYPE_2D); CHECK_EQ(im->ici.format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(im->ici.extent.width, 8); CHECK_EQ(im->ici.extent.height, 4);
    CHECK_EQ(im->ici.extent.depth, 1); CHECK_EQ(im->ici.mipLevels, 2); CHECK_EQ(im->ici.arrayLayers, 1); CHECK_EQ(im->ici.samples, VK_SAMPLE_COUNT_1_BIT);
    CHECK_EQ(im->ici.tiling, VK_IMAGE_TILING_OPTIMAL); CHECK_EQ(im->ici.initialLayout, VK_IMAGE_LAYOUT_UNDEFINED); CHECK_EQ(im->ici.flags, 0);
    CHECK_EQ(im->ici.usage, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    mk_obj *mem = mk_obj_of(im->bound); CHECK(mem && mem->mem_type == 0);   // device-local
    CHECK(t->bytes == (mem ? mem->size : 0));
    VkImageView vs[16]; CHECK_EQ(mk_views_of(t->image, vs, 16), 1); CHECK(vs[0] == t->view); CHECK(t->att == VK_NULL_HANDLE);
    mk_obj *v = view_of(t->view);
    if (v) { CHECK_EQ(v->vci.viewType, VK_IMAGE_VIEW_TYPE_2D); CHECK_EQ(v->vci.subresourceRange.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
             CHECK_EQ(v->vci.subresourceRange.levelCount, 2); CHECK_EQ(v->vci.subresourceRange.layerCount, 1); CHECK_EQ(v->vci.components.r, VK_COMPONENT_SWIZZLE_IDENTITY); }
    CHECK(vkn_tex_get(H(5, 1)) == t); CHECK(vkn_tex_get(H(5, 2)) == NULL); CHECK(vkn_tex_by_cookie(0xC2D) == t);
    vkn_tex *tt = NULL; CHECK(vkn_tex_view_by_id(100, &tt) == t->view); CHECK(tt == t); CHECK(vkn_tex_view_by_id(101, &tt) == VK_NULL_HANDLE && tt == NULL);
    CHECK_EQ(t->layout, VK_IMAGE_LAYOUT_UNDEFINED);
    flush_logged();
    CHECK_EQ(copies_of(t->image), 2);
    const mk_copy_t *c0 = copy_of(t->image, 0, 0, 0), *c1 = copy_of(t->image, 1, 0, 0);
    CHECK(c0 && c1);
    if (c0 && c1) {
        CHECK_EQ(c0->c.imageExtent.width, 8); CHECK_EQ(c0->c.imageExtent.height, 4); CHECK_EQ(c0->c.imageExtent.depth, 1);
        CHECK_EQ(c1->c.imageExtent.width, 4); CHECK_EQ(c1->c.imageExtent.height, 2);
        CHECK_EQ(c0->c.bufferRowLength, 0); CHECK_EQ(c0->c.imageSubresource.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT); CHECK_EQ(c0->c.imageSubresource.layerCount, 1);
        CHECK_EQ(c1->c.bufferOffset - c0->c.bufferOffset, 128); CHECK_EQ(c0->c.bufferOffset % 16, 0);
        CHECK(c0->src && !memcmp(c0->src, e0, sizeof e0)); CHECK(c1->src && !memcmp(c1->src, e1, sizeof e1));
    }
    CHECK_EQ(bars_of(t->image), 2);
    const mk_bar_t *b0 = bar_of(t->image, 0), *b1 = bar_of(t->image, 1);
    if (b0 && b1) {
        CHECK_EQ(b0->from, VK_IMAGE_LAYOUT_UNDEFINED); CHECK_EQ(b0->to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CHECK_EQ(b1->from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); CHECK_EQ(b1->to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK_EQ(b0->r.levelCount, 2); CHECK_EQ(b0->r.layerCount, 1); CHECK_EQ(b0->r.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    CHECK_EQ(t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frames(2); CHECK_EQ(used_of(VKN_ARENA_STAGING), st0 + 160);
    frames(1); CHECK_EQ(used_of(VKN_ARENA_STAGING), st0);   // the staging block retired with the upload
    // destroy: out of the tables at once, the Vulkan objects 3 frame ends later
    const VkImage img = t->image; const VkImageView view = t->view; const uint64_t memh = im->bound;
    CHECK_EQ(vkn_tex_destroy_cookie(0xC2D), 1); CHECK_EQ(vkn_tex_destroy_cookie(0xC2D), 0);
    CHECK(vkn_tex_get(H(5, 1)) == NULL); CHECK(vkn_tex_view_by_id(100, NULL) == VK_NULL_HANDLE); CHECK(vkn_tex_by_cookie(0xC2D) == NULL);
    frames(2); CHECK(mk_is_live((uint64_t)(uintptr_t)img, MK_IMAGE)); CHECK(mk_is_live((uint64_t)(uintptr_t)view, MK_VIEW));
    frames(1); CHECK(!mk_is_live((uint64_t)(uintptr_t)img, MK_IMAGE)); CHECK(!mk_is_live((uint64_t)(uintptr_t)view, MK_VIEW)); CHECK(!mk_is_live(memh, MK_MEMORY));

    // mips beyond the chain are clamped; MS has one level
    vkn_tex_desc dm = tdesc(4, 1, 8, 8, 10, 1, VKN_TC_SAMPLED, 0xC3, ~0u);
    t = vkn_tex_create(H(6, 1), &dm, NULL, NULL); CHECK(t && t->mips == 4); if (t) { im = img_of(t); CHECK(im && im->ici.mipLevels == 4); }
    vkn_tex_destroy_cookie(0xC3);

    // a cube: 6 faces, per-face ids -> 2D views of one layer
    uint8_t face[6][4 * 4 * 4];
    vkn_tex_region fr[6];
    for (int f = 0; f < 6; f++) { memset(face[f], f + 1, sizeof face[f]); fr[f] = (vkn_tex_region){0, (uint32_t)f, 0, 4, 4, 1, face[f]}; }
    vkn_tex_src fsrc = {VKN_SRC_REGIONS, fr, 6, NULL, 0, 0, 0};
    vkn_tex_desc dc = tdesc(4, 8, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xCC, 199);
    for (int f = 0; f < 6; f++) dc.layer_ids[f] = 200 + f;
    t = vkn_tex_create(H(7, 1), &dc, &fsrc, NULL);
    CHECK(t != NULL);
    if (t) {
        im = img_of(t);
        CHECK(im && im->ici.arrayLayers == 6 && (im->ici.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && im->ici.imageType == VK_IMAGE_TYPE_2D);
        CHECK_EQ(t->cube, 1); CHECK_EQ(t->layers, 6);
        v = view_of(t->view); CHECK(v && v->vci.viewType == VK_IMAGE_VIEW_TYPE_CUBE && v->vci.subresourceRange.layerCount == 6);
        CHECK_EQ(mk_views_of(t->image, vs, 16), 7);
        for (int f = 0; f < 6; f++) {
            mk_obj *lv = view_of(t->layer_view[f]);
            CHECK(lv && lv->vci.viewType == VK_IMAGE_VIEW_TYPE_2D && lv->vci.subresourceRange.baseArrayLayer == (uint32_t)f && lv->vci.subresourceRange.layerCount == 1);
            CHECK(vkn_tex_view_by_id(200 + f, &tt) == t->layer_view[f] && tt == t);
        }
        CHECK(t->layer_view[6] == VK_NULL_HANDLE);
        CHECK(vkn_tex_view_by_id(199, NULL) == t->view);
        flush_logged();
        CHECK_EQ(copies_of(t->image), 6);
        for (int f = 0; f < 6; f++) {
            const mk_copy_t *c = copy_of(t->image, 0, (uint32_t)f, 0);
            CHECK(c && c->src && c->src[0] == f + 1 && c->src[63] == f + 1 && c->c.imageSubresource.layerCount == 1);
        }
        const mk_bar_t *b = bar_of(t->image, 0); CHECK(b && b->r.layerCount == 6);
        vkn_tex_destroy_cookie(0xCC);
        CHECK(vkn_tex_view_by_id(203, NULL) == VK_NULL_HANDLE);
    }
    // a "cube" that is not square cannot be cube compatible: a 2D array of 6
    vkn_tex_desc dn = tdesc(4, 8, 8, 4, 1, 1, VKN_TC_SAMPLED, 0xCD, ~0u);
    t = vkn_tex_create(H(8, 1), &dn, NULL, NULL);
    CHECK(t != NULL); if (t) { im = img_of(t); CHECK(im && !(im->ici.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)); v = view_of(t->view); CHECK(v && v->vci.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY); }
    vkn_tex_destroy_cookie(0xCD);
    // a 2D array of 3 with per-layer ids
    uint8_t al[3][8 * 8 * 4]; vkn_tex_region ar[3];
    for (int l = 0; l < 3; l++) { memset(al[l], 0x40 + l, sizeof al[l]); ar[l] = (vkn_tex_region){0, (uint32_t)l, 0, 8, 8, 1, al[l]}; }
    vkn_tex_src asrc = {VKN_SRC_REGIONS, ar, 3, NULL, 0, 0, 0};
    vkn_tex_desc da = tdesc(4, 4, 8, 8, 1, 3, VKN_TC_SAMPLED, 0xCA, 299);
    for (int l = 0; l < 3; l++) da.layer_ids[l] = 300 + l;
    t = vkn_tex_create(H(9, 1), &da, &asrc, NULL);
    CHECK(t != NULL);
    if (t) {
        im = img_of(t); CHECK(im && im->ici.arrayLayers == 3 && !im->ici.flags);
        v = view_of(t->view); CHECK(v && v->vci.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY && v->vci.subresourceRange.layerCount == 3);
        CHECK(vkn_tex_view_by_id(302, &tt) == t->layer_view[2] && t->layer_view[2]); CHECK(t->layer_view[3] == VK_NULL_HANDLE);
        flush_logged();
        CHECK_EQ(copies_of(t->image), 3);
        for (int l = 0; l < 3; l++) { const mk_copy_t *c = copy_of(t->image, 0, (uint32_t)l, 0); CHECK(c && c->src && c->src[255] == 0x40 + l); }
        vkn_tex_destroy_cookie(0xCA);
    }
    // a 3D texture: depth slices per region (z, d), no layer views
    uint8_t vol0[4 * 4 * 2 * 4], vol1[4 * 4 * 2 * 4], volm[2 * 2 * 2 * 4];
    memset(vol0, 1, sizeof vol0); memset(vol1, 2, sizeof vol1); memset(volm, 3, sizeof volm);
    vkn_tex_region vr[3] = {{0, 0, 0, 4, 4, 2, vol0}, {0, 0, 2, 4, 4, 2, vol1}, {1, 0, 0, 2, 2, 2, volm}};
    vkn_tex_src vsrc = {VKN_SRC_REGIONS, vr, 3, NULL, 0, 0, 0};
    vkn_tex_desc d3 = tdesc(4, 2, 4, 4, 2, 1, VKN_TC_SAMPLED, 0xC3D, ~0u); d3.depth = 4; d3.layer_ids[0] = 400;
    t = vkn_tex_create(H(10, 1), &d3, &vsrc, NULL);
    CHECK(t != NULL);
    if (t) {
        im = img_of(t); CHECK(im && im->ici.imageType == VK_IMAGE_TYPE_3D && im->ici.extent.depth == 4 && im->ici.arrayLayers == 1);
        v = view_of(t->view); CHECK(v && v->vci.viewType == VK_IMAGE_VIEW_TYPE_3D); CHECK(t->layer_view[0] == VK_NULL_HANDLE);
        flush_logged();
        const mk_copy_t *a0 = copy_of(t->image, 0, 0, 0), *a2 = copy_of(t->image, 0, 0, 2), *m1 = copy_of(t->image, 1, 0, 0);
        CHECK(a0 && a0->c.imageExtent.depth == 2 && a0->src && a0->src[0] == 1);
        CHECK(a2 && a2->c.imageExtent.depth == 2 && a2->src && a2->src[127] == 2);
        CHECK(m1 && m1->c.imageExtent.width == 2 && m1->c.imageExtent.depth == 2 && m1->src && m1->src[31] == 3);
        vkn_tex_destroy_cookie(0xC3D);
    }
    // 1D: height 1 whatever the descriptor says
    vkn_tex_desc d1 = tdesc(4, 0, 16, 5, 1, 1, VKN_TC_SAMPLED, 0xC1D, ~0u);
    t = vkn_tex_create(H(11, 1), &d1, NULL, NULL);
    CHECK(t != NULL); if (t) { im = img_of(t); CHECK(im && im->ici.imageType == VK_IMAGE_TYPE_1D && im->ici.extent.height == 1); v = view_of(t->view); CHECK(v && v->vci.viewType == VK_IMAGE_VIEW_TYPE_1D); }
    vkn_tex_destroy_cookie(0xC1D);
    flush_logged(); frames(3);
}

static void test_tex_widen_bc(void) {
    // RGB8 on a device that does not sample it: widened to RGBA8 with alpha 0xff; RGB32F: alpha 1.0f
    mk_rgb3 = 0;
    uint8_t rgb[2 * 2 * 3]; for (int i = 0; i < 12; i++) rgb[i] = (uint8_t)(10 + i);
    vkn_tex_region r1 = {0, 0, 0, 2, 2, 1, rgb};
    vkn_tex_src s1 = {VKN_SRC_REGIONS, &r1, 1, NULL, 0, 0, 0};
    vkn_tex_desc d = tdesc(3, 1, 2, 2, 1, 1, VKN_TC_SAMPLED, 0xD1, ~0u);
    vkn_tex *t = vkn_tex_create(H(20, 1), &d, &s1, NULL);
    CHECK(t != NULL);
    if (t) {
        CHECK_EQ(t->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(t->widened, 1);
        flush_logged(); const mk_copy_t *c = copy_of(t->image, 0, 0, 0);
        CHECK(c && c->src);
        if (c && c->src) for (int p = 0; p < 4; p++) { CHECK(!memcmp(c->src + 4 * p, rgb + 3 * p, 3)); CHECK_EQ(c->src[4 * p + 3], 0xff); }
        vkn_tex_destroy_cookie(0xD1);
    }
    float rgbf[2 * 2 * 3]; for (int i = 0; i < 12; i++) rgbf[i] = (float)i * 0.5f;
    vkn_tex_region r2 = {0, 0, 0, 2, 2, 1, rgbf};
    vkn_tex_src s2 = {VKN_SRC_REGIONS, &r2, 1, NULL, 0, 0, 0};
    d = tdesc(23, 1, 2, 2, 1, 1, VKN_TC_SAMPLED, 0xD2, ~0u);
    t = vkn_tex_create(H(21, 1), &d, &s2, NULL);
    CHECK(t != NULL);
    if (t) {
        CHECK_EQ(t->format, VK_FORMAT_R32G32B32A32_SFLOAT);
        flush_logged(); const mk_copy_t *c = copy_of(t->image, 0, 0, 0);
        if (c && c->src) { const float *o = (const float *)c->src; for (int p = 0; p < 4; p++) { CHECK(o[4 * p] == rgbf[3 * p] && o[4 * p + 2] == rgbf[3 * p + 2]); CHECK(o[4 * p + 3] == 1.0f); } }
        else CHECK(0);
        vkn_tex_destroy_cookie(0xD2);
    }
    // RGB8 on a device that samples it: kept 3 bytes a texel; every bufferOffset a multiple of 12 and 4 (48), 2 levels
    mk_rgb3 = 1;
    uint8_t m0[4 * 4 * 3], m1[2 * 2 * 3]; memset(m0, 0x11, sizeof m0); memset(m1, 0x22, sizeof m1);
    vkn_tex_region r3[2] = {{0, 0, 0, 4, 4, 1, m0}, {1, 0, 0, 2, 2, 1, m1}};
    vkn_tex_src s3 = {VKN_SRC_REGIONS, r3, 2, NULL, 0, 0, 0};
    for (int e = 0; e < 3; e++) {   // RGB8 (3 bytes), RGB16F (6), RGB32F (12): offsets multiple of 48
        static const uint32_t ef[3] = {3, 19, 23};
        d = tdesc(ef[e], 1, 4, 4, 2, 1, VKN_TC_SAMPLED, 0xD3 + e, ~0u);
        const char *why = NULL;
        // (data sizes: RGB8 regions as above; for the wider formats the region pointers need more bytes)
        static uint8_t big0[4 * 4 * 12], big1[2 * 2 * 12];
        vkn_tex_region rb[2] = {{0, 0, 0, 4, 4, 1, e ? (const void *)big0 : m0}, {1, 0, 0, 2, 2, 1, e ? (const void *)big1 : m1}};
        vkn_tex_src sb = {VKN_SRC_REGIONS, rb, 2, NULL, 0, 0, 0};
        t = vkn_tex_create(H(22 + e, 1), &d, e ? &sb : &s3, &why);
        CHECK(t != NULL);
        if (!t) { printf("  3-component engine format %u: %s\n", ef[e], why ? why : "?"); continue; }
        int x; CHECK_EQ(t->format, exp_nvn_vk(nr_formats[ef[e]].nvn, &x)); CHECK_EQ(t->widened, 0);
        flush_logged();
        const mk_copy_t *c0 = copy_of(t->image, 0, 0, 0), *c1 = copy_of(t->image, 1, 0, 0);
        CHECK(c0 && c1);
        if (c0 && c1) { CHECK_EQ(c0->c.bufferOffset % 48, 0); CHECK_EQ(c1->c.bufferOffset % 48, 0); if (!e) CHECK(c0->src[47] == 0x11 && c1->src[11] == 0x22); }
        vkn_tex_destroy_cookie(0xD3 + e);
    }
    mk_rgb3 = 0;
    // BC1 without BC sampling: decoded to RGBA8 (8x8 level white, 4x4 red, 2x2 blue: the edge block decoded into 2x2 pixels)
    mk_bc = 0;
    uint8_t lv0[4 * 8], lv1[8], lv2[8];
    for (int b = 0; b < 4; b++) bc1_solid(lv0 + 8 * b, 0xffff);
    bc1_solid(lv1, 0xf800); bc1_solid(lv2, 0x001f);
    vkn_tex_region br[3] = {{0, 0, 0, 8, 8, 1, lv0}, {1, 0, 0, 4, 4, 1, lv1}, {2, 0, 0, 2, 2, 1, lv2}};
    vkn_tex_src bsrc = {VKN_SRC_REGIONS, br, 3, NULL, 0, 0, 0};
    d = tdesc(53, 1, 8, 8, 3, 1, VKN_TC_SAMPLED, 0xD8, 800);
    const uint64_t pend0 = vkn_upload_pending_bytes();
    t = vkn_tex_create(H(30, 1), &d, &bsrc, NULL);
    CHECK(t != NULL);
    if (t) {
        CHECK_EQ(t->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(t->decoded, 1); CHECK_EQ(t->mips, 3);
        mk_obj *im = img_of(t); CHECK(im && im->ici.format == VK_FORMAT_R8G8B8A8_UNORM && im->ici.mipLevels == 3);
        CHECK_EQ(vkn_upload_pending_bytes(), pend0 + 256 + 64 + 16);
        flush_logged();
        const mk_copy_t *c0 = copy_of(t->image, 0, 0, 0), *c1 = copy_of(t->image, 1, 0, 0), *c2 = copy_of(t->image, 2, 0, 0);
        CHECK(c0 && c1 && c2);
        if (c0 && c1 && c2) {
            CHECK(all_rgba(c0->src, 64, 255, 255, 255, 255)); CHECK(all_rgba(c1->src, 16, 255, 0, 0, 255)); CHECK(all_rgba(c2->src, 4, 0, 0, 255, 255));
            CHECK_EQ(c2->c.imageExtent.width, 2); CHECK_EQ(c2->c.imageExtent.height, 2);
            CHECK_EQ(c1->c.bufferOffset - c0->c.bufferOffset, 256); CHECK_EQ(c2->c.bufferOffset - c1->c.bufferOffset, 64);
        }
        vkn_tex_destroy_cookie(0xD8);
    }
    // BC6H without BC sampling: RGBA16F with alpha 1.0 (half 0x3c00)
    uint8_t b6[16]; memset(b6, 0, sizeof b6);
    vkn_tex_region r6 = {0, 0, 0, 4, 4, 1, b6};
    vkn_tex_src s6 = {VKN_SRC_REGIONS, &r6, 1, NULL, 0, 0, 0};
    d = tdesc(58, 1, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xD9, ~0u);
    t = vkn_tex_create(H(31, 1), &d, &s6, NULL);
    CHECK(t != NULL);
    if (t) {
        CHECK_EQ(t->format, VK_FORMAT_R16G16B16A16_SFLOAT);
        flush_logged(); const mk_copy_t *c = copy_of(t->image, 0, 0, 0);
        if (c && c->src) { const uint16_t *hp = (const uint16_t *)c->src; int ok = 1; for (int p = 0; p < 16; p++) ok &= hp[4 * p + 3] == 0x3c00; CHECK(ok); } else CHECK(0);
        vkn_tex_destroy_cookie(0xD9);
    }
    // the same BC1 texture on a device that samples BC: native blocks, copied as they are
    mk_bc = 1;
    for (int b = 0; b < 4; b++) bc1_solid(lv0 + 8 * b, 0xffff);
    d = tdesc(53, 1, 8, 8, 3, 1, VKN_TC_SAMPLED, 0xDA, ~0u);
    t = vkn_tex_create(H(32, 1), &d, &bsrc, NULL);
    CHECK(t != NULL);
    if (t) {
        CHECK_EQ(t->format, VK_FORMAT_BC1_RGB_UNORM_BLOCK); CHECK_EQ(t->decoded, 0);
        flush_logged();
        const mk_copy_t *c0 = copy_of(t->image, 0, 0, 0), *c2 = copy_of(t->image, 2, 0, 0);
        CHECK(c0 && c2);
        if (c0 && c2) { CHECK(!memcmp(c0->src, lv0, 32)); CHECK(!memcmp(c2->src, lv2, 8)); CHECK_EQ(c0->c.bufferOffset % 16, 0); CHECK_EQ(c2->c.bufferOffset % 16, 0);
                        CHECK_EQ(c2->c.imageExtent.width, 2); }
        vkn_tex_destroy_cookie(0xDA);
    }
    flush_logged(); frames(3);
}

// A packaged (block-linear) texture: storage written with the hand GOB formula, read back linear through the deswizzle.
static void test_tex_packaged(void) {
    mk_bc = 1;
    nvn_texdesc nd; memset(&nd, 0, sizeof nd);
    nd.target = 1; nd.format = 0x25; nd.width = 32; nd.height = 8; nd.depth = 1; nd.levels = 2; nd.pkg_block_h = 1;
    nvn_texlayout L; nvn_tex_layout(&nd, &L);
    CHECK(L.size >= 1024 + 256);
    uint8_t *stor = (uint8_t *)calloc(1, (size_t)L.size + 64);
    uint8_t lin[2][32 * 8 * 4]; const uint32_t lw[2] = {32, 16}, lh[2] = {8, 4};
    for (unsigned l = 0; l < 2; l++) {
        const uint64_t base = nvn_tex_level_offset(&nd, l); unsigned th, td; nvn_tex_level_tiles(&nd, l, &th, &td);
        const uint32_t rb = lw[l] * 4;
        for (uint32_t y = 0; y < lh[l]; y++) for (uint32_t x = 0; x < rb; x++) {
            const uint8_t v = (uint8_t)(x * 3 + y * 17 + l * 101 + 5);
            lin[l][y * rb + x] = v;
            const uint64_t o = bl_off(x, y, rb, th);
            if ((x % 13) == 0) CHECK_EQ(o, nvn_bl_offset(x, y, 0, rb, lh[l], th, td));   // the hand formula = the module's address function
            stor[base + o] = v;
        }
    }
    vkn_tex_src src = {VKN_SRC_PACKAGED, NULL, 0, stor, L.size, 1, 1};
    vkn_tex_desc d = tdesc(4, 1, 32, 8, 2, 1, VKN_TC_SAMPLED, 0xE1, 900);
    vkn_tex *t = vkn_tex_create(H(40, 1), &d, &src, NULL);
    CHECK(t != NULL);
    if (t) {
        memset(stor, 0, (size_t)L.size);   // copied at creation
        flush_logged();
        CHECK_EQ(copies_of(t->image), 2);
        const mk_copy_t *c0 = copy_of(t->image, 0, 0, 0), *c1 = copy_of(t->image, 1, 0, 0);
        CHECK(c0 && c1);
        if (c0 && c1) { CHECK(!memcmp(c0->src, lin[0], 32 * 8 * 4)); CHECK(!memcmp(c1->src, lin[1], 16 * 4 * 4)); CHECK_EQ(c1->c.imageExtent.width, 16); CHECK_EQ(c1->c.imageExtent.height, 4); }
        vkn_tex_stats ts; vkn_tex_get_stats(&ts); CHECK(ts.deswizzled >= 1);
        vkn_tex_destroy_cookie(0xE1);
    }
    // storage smaller than the layout: refused
    vkn_tex_src small = {VKN_SRC_PACKAGED, NULL, 0, stor, L.size - 1, 1, 1};
    const char *why = NULL;
    CHECK(vkn_tex_create(H(41, 1), &d, &small, &why) == NULL); CHECK(why && !strcmp(why, "packaged storage smaller than its layout"));
    free(stor);
    // a packaged BC1 texture on a device without BC: deswizzled block rows, then decoded (16x16 = 4 x 4 blocks of 8 bytes)
    mk_bc = 0;
    memset(&nd, 0, sizeof nd); nd.target = 1; nd.format = 0x41; nd.width = 16; nd.height = 16; nd.depth = 1; nd.levels = 1; nd.pkg_block_h = 1;
    nvn_tex_layout(&nd, &L);
    stor = (uint8_t *)calloc(1, (size_t)L.size);
    static const uint16_t col[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
    unsigned th, td; nvn_tex_level_tiles(&nd, 0, &th, &td);
    for (uint32_t by = 0; by < 4; by++) for (uint32_t bx = 0; bx < 4; bx++) {
        uint8_t blk[8]; bc1_solid(blk, col[(bx + by) % 4]);
        for (uint32_t i = 0; i < 8; i++) stor[bl_off(bx * 8 + i, by, 32, th)] = blk[i];
    }
    vkn_tex_src bsrc = {VKN_SRC_PACKAGED, NULL, 0, stor, L.size, 1, 1};
    d = tdesc(53, 1, 16, 16, 1, 1, VKN_TC_SAMPLED, 0xE2, ~0u);
    t = vkn_tex_create(H(42, 1), &d, &bsrc, NULL);
    CHECK(t != NULL);
    if (t) {
        CHECK_EQ(t->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(t->decoded, 1);
        flush_logged(); const mk_copy_t *c = copy_of(t->image, 0, 0, 0);
        CHECK(c && c->src);
        if (c && c->src) {
            int ok = 1;
            for (uint32_t y = 0; y < 16; y++) for (uint32_t x = 0; x < 16; x++) {
                const uint16_t k = col[(x / 4 + y / 4) % 4]; const uint8_t *p = c->src + (y * 16 + x) * 4;
                ok &= p[0] == ((k & 0xf800) ? 255 : 0) && p[1] == ((k & 0x07e0) ? 255 : 0) && p[2] == ((k & 0x001f) ? 255 : 0) && p[3] == 255;
            }
            CHECK(ok);
        }
        vkn_tex_destroy_cookie(0xE2);
    }
    free(stor);
    mk_bc = 1;
    flush_logged(); frames(3);
}

// ---- stage 4: the setup ASTC pack (vkn_astc.c) on a device without BC ----
// The key function of the dos2 build lives in xbg_game.cpp (zlib); the test gives its own CRC-32 / Adler-32 (checked against the
// published check values), written here from the definitions.
static uint32_t t_crc32(const uint8_t *p, uint64_t n) {
    uint32_t c = 0xffffffffu;
    for (uint64_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1; }
    return ~c;
}
static uint32_t t_adler32(const uint8_t *p, uint64_t n) {
    uint32_t a = 1, b = 0;
    for (uint64_t i = 0; i < n; i++) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
    return b << 16 | a;
}
static int g_key_calls;
int vkn_shlib_astc_key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) {   // vkn_shlib.cpp (the pack key)
    g_key_calls++; *crc = t_crc32((const uint8_t *)mem, size); *adler = t_adler32((const uint8_t *)mem, size); return 0;
}
typedef struct { vkn_astc_entry e; const uint8_t *data; } tp_ent;
static int tp_less(const void *a, const void *b) {   // the tool's sort key: crc, adler, bytes, format, width, height, levels
    const vkn_astc_entry *x = &((const tp_ent *)a)->e, *y = &((const tp_ent *)b)->e;
    const uint64_t kx[7] = {x->crc, x->adler, x->bytes, x->format, x->width, x->height, x->levels}, ky[7] = {y->crc, y->adler, y->bytes, y->format, y->width, y->height, y->levels};
    for (int i = 0; i < 7; i++) if (kx[i] != ky[i]) return kx[i] < ky[i] ? -1 : 1;
    return 0;
}
// Writes a D3AC v1 pack: header, the entries' data in their order, the sorted index. data_bytes as given (a wrong one tests the
// reader); the data written is data_bytes long.
static void tp_write(const char *path, tp_ent *e, uint32_t n, const char *magic) {
    FILE *f = fopen(path, "wb"); CHECK(f != NULL); if (!f) return;
    vkn_astc_hdr h; memset(&h, 0, sizeof h); memcpy(h.magic, magic, 4); h.version = 1; h.entries = n; h.data_off = 64;
    fwrite(&h, 64, 1, f);
    uint64_t off = 64;
    for (uint32_t i = 0; i < n; i++) { e[i].e.data_off = off; fwrite(e[i].data, 1, e[i].e.data_bytes, f); off += e[i].e.data_bytes; }
    h.data_bytes = off - 64; h.index_off = off;
    qsort(e, n, sizeof *e, tp_less);
    for (uint32_t i = 0; i < n; i++) fwrite(&e[i].e, sizeof e[i].e, 1, f);
    fseek(f, 0, SEEK_SET); fwrite(&h, 64, 1, f); fclose(f);
}
static tp_ent tp_of(const uint8_t *stor, uint64_t bytes, uint16_t fmt, uint16_t w, uint16_t h, uint16_t levels, uint8_t b, const uint8_t *data, uint32_t data_bytes) {
    tp_ent t; memset(&t, 0, sizeof t);
    t.e.crc = t_crc32(stor, bytes); t.e.adler = t_adler32(stor, bytes); t.e.bytes = (uint32_t)bytes; t.e.format = fmt; t.e.width = w; t.e.height = h; t.e.levels = levels;
    t.e.bw = t.e.bh = b; t.e.data_bytes = data_bytes; t.e.psnr_x100 = 4500; t.data = data;
    return t;
}
// Packaged storage of a 2D texture: the bytes are a pattern (a hit never decodes them; a miss decodes them as whatever BC they are).
static uint8_t *tp_storage(uint32_t nvn, uint32_t w, uint32_t h, uint32_t levels, uint32_t target, uint8_t seed, uint64_t *bytes) {
    nvn_texdesc nd; memset(&nd, 0, sizeof nd); nd.target = (int)target; nd.format = (int)nvn; nd.width = w; nd.height = h; nd.depth = 1; nd.levels = levels; nd.pkg_block_h = 1;
    nvn_texlayout L; nvn_tex_layout(&nd, &L);
    uint8_t *s = (uint8_t *)malloc((size_t)L.size);
    for (uint64_t i = 0; s && i < L.size; i++) s[i] = (uint8_t)(i * 7 + seed);
    *bytes = L.size; return s;
}
static vkn_tex *tp_create(uint64_t handle, uint32_t nvn, uint32_t w, uint32_t h, uint32_t levels, uint32_t target, const uint8_t *stor, uint64_t bytes, uint64_t cookie,
                          uint32_t pool, const char **why) {
    vkn_tex_desc d = tdesc(55, target, w, h, levels, 1, VKN_TC_SAMPLED, cookie, pool); d.nvn_fmt = nvn;
    vkn_tex_src s = {VKN_SRC_PACKAGED, NULL, 0, stor, bytes, 1, 1};
    return vkn_tex_create(handle, &d, &s, why);
}
static void test_tex_astc_pack(void) {
    static const char *const path = "/tmp/vkn_stage2_astc.pack";
    // the test's key function = zlib's (check values of CRC-32 and Adler-32)
    CHECK_EQ(t_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u); CHECK_EQ(t_adler32((const uint8_t *)"Wikipedia", 9), 0x11E60398u);
    // pure pieces
    vkn_astc_entry pe; memset(&pe, 0, sizeof pe); pe.width = 20; pe.height = 12; pe.bw = pe.bh = 6; pe.levels = 3;
    CHECK_EQ(vkn_astc_level_bytes(&pe, 0), 4 * 2 * 16); CHECK_EQ(vkn_astc_level_bytes(&pe, 1), 2 * 1 * 16); CHECK_EQ(vkn_astc_level_bytes(&pe, 4), 16);
    CHECK_EQ(vkn_astc_vkformat(4, 4, 1), VK_FORMAT_ASTC_4x4_SRGB_BLOCK); CHECK_EQ(vkn_astc_vkformat(5, 5, 0), VK_FORMAT_ASTC_5x5_UNORM_BLOCK);
    CHECK_EQ(vkn_astc_vkformat(6, 6, 1), VK_FORMAT_ASTC_6x6_SRGB_BLOCK); CHECK_EQ(vkn_astc_vkformat(8, 8, 0), VK_FORMAT_UNDEFINED);
    CHECK(vkn_astc_srgb(0x48) && vkn_astc_srgb(0x45) && !vkn_astc_srgb(0x44) && !vkn_astc_srgb(0x41));
    CHECK(vkn_astc_cacheable(0x44, 1, 1, 1, 4096, 64, 64)); CHECK(!vkn_astc_cacheable(0x49, 1, 1, 1, 4096, 64, 64)); CHECK(!vkn_astc_cacheable(0x44, 8, 6, 1, 4096, 64, 64));
    CHECK(!vkn_astc_cacheable(0x44, 1, 2, 1, 4096, 64, 64)); CHECK(!vkn_astc_cacheable(0x44, 1, 1, 4, 4096, 64, 64)); CHECK(!vkn_astc_cacheable(0x44, 1, 1, 1, 4096, 70000, 64));
    { uint8_t z[48]; memset(z, 0, sizeof z); z[20] = 1; CHECK_EQ(vkn_astc_sanitize(z, 48), 2); CHECK(z[0] == 0xFC && z[1] == 0xFD && z[7] == 0xFF && z[8] == 0 && z[20] == 1 && z[32] == 0xFC); }

    // storages: A BC3 16x16 x 3 levels, B the same shape with other bytes, C BC1 20x20 (5x5 entry), D BC1 12x12 x 2 levels (6x6 entry)
    uint64_t na, nb, nc, nd;
    uint8_t *sa = tp_storage(0x44, 16, 16, 3, 1, 3, &na), *sb = tp_storage(0x44, 16, 16, 3, 1, 9, &nb), *sc = tp_storage(0x42, 20, 20, 1, 1, 5, &nc),
            *sd = tp_storage(0x42, 12, 12, 2, 1, 1, &nd);
    CHECK(sa && sb && sc && sd); if (!sa || !sb || !sc || !sd) { free(sa); free(sb); free(sc); free(sd); return; }
    CHECK_EQ(na, nb);
    uint8_t da[336], dc[256], dd[80];   // A: 4x4 blocks 256 + 64 + 16; C: 5x5 blocks 4 x 4 x 16; D: 6x6 blocks 2 x 2 x 16 + 16
    for (unsigned i = 0; i < sizeof da; i++) da[i] = (uint8_t)((i * 13 + 1) | 1);
    memset(da + 256, 0, 16);   // an all-zero block in level 1: arrives as the void extent of transparent black
    for (unsigned i = 0; i < sizeof dc; i++) dc[i] = (uint8_t)(i | 0x80);
    for (unsigned i = 0; i < sizeof dd; i++) dd[i] = (uint8_t)(255 - i);
    uint8_t ea[336]; memcpy(ea, da, sizeof ea); vkn_astc_sanitize(ea, sizeof ea);
    tp_ent ents[5] = {tp_of(sa, na, 0x44, 16, 16, 3, 4, da, 336), tp_of(sa, na, 0x48, 16, 16, 3, 4, da, 336), tp_of(sb, nb, 0x44, 16, 16, 3, 4, da, 336),
                      tp_of(sc, nc, 0x42, 20, 20, 1, 5, dc, 256), tp_of(sd, nd, 0x42, 12, 12, 2, 6, dd, 80)};
    ents[2].e.bytes = (uint32_t)nb + 16;   // B's CRC / Adler, another byte count: B must miss
    tp_write(path, ents, 5, "D3AC");
    // the pure lookup over the written (sorted) index
    { vkn_astc_entry idx[5]; for (int i = 0; i < 5; i++) idx[i] = ents[i].e;
      CHECK(vkn_astc_find(idx, 5, t_crc32(sa, na), t_adler32(sa, na), (uint32_t)na, 0x48, 3, 16, 16) >= 0);
      CHECK_EQ(vkn_astc_find(idx, 5, t_crc32(sa, na), t_adler32(sa, na), (uint32_t)na, 0x48, 2, 16, 16), -1);
      CHECK_EQ(vkn_astc_find(idx, 5, t_crc32(sb, nb), t_adler32(sb, nb), (uint32_t)nb, 0x44, 3, 16, 16), -1); }

    setenv("DM_ASTC_CACHE_PATH", path, 1); unsetenv("DM_VK_NATIVE_ASTC");
    mk_bc = 0; mk_astc = 5;   // no BC; ASTC 4x4 and 6x6, no 5x5
    vkn_astc_reset();
    vkn_tex_stats s0; vkn_tex_get_stats(&s0);
    const uint64_t pend0 = vkn_upload_pending_bytes();
    const char *why = NULL;
    // hit: ASTC 4x4 UNORM, every level from the pack, the zero block fixed, no CPU decode
    vkn_tex *A = tp_create(H(300, 1), 0x44, 16, 16, 3, 1, sa, na, 0x3A0, 3000, &why);
    CHECK(A != NULL);
    vkn_astc_stats as; vkn_astc_get_stats(&as);
    CHECK_EQ(as.state, VKN_ASTC_READY); CHECK_EQ(as.entries, 5); CHECK_EQ(as.block_ok[0], 1); CHECK_EQ(as.block_ok[1], 0); CHECK_EQ(as.block_ok[2], 1);
    if (A) {
        CHECK_EQ(A->format, VK_FORMAT_ASTC_4x4_UNORM_BLOCK); CHECK_EQ(A->astc, 4); CHECK_EQ(A->decoded, 0); CHECK_EQ(A->mips, 3);
        mk_obj *im = img_of(A); CHECK(im && im->ici.format == VK_FORMAT_ASTC_4x4_UNORM_BLOCK && im->ici.mipLevels == 3);
        CHECK_EQ(vkn_upload_pending_bytes(), pend0 + 336);
        CHECK_EQ(as.hits, 1); CHECK_EQ(as.reads, 1); CHECK_EQ(as.read_bytes, 336); CHECK_EQ(as.zero_fixed, 1); CHECK_EQ(as.read_fails, 0); CHECK_EQ(as.hash_bytes, na);
        flush_logged();
        CHECK_EQ(copies_of(A->image), 3);
        const mk_copy_t *c0 = copy_of(A->image, 0, 0, 0), *c1 = copy_of(A->image, 1, 0, 0), *c2 = copy_of(A->image, 2, 0, 0);
        CHECK(c0 && c1 && c2);
        if (c0 && c1 && c2 && c0->src && c1->src && c2->src) {
            CHECK(!memcmp(c0->src, ea, 256)); CHECK(!memcmp(c1->src, ea + 256, 64)); CHECK(!memcmp(c2->src, ea + 320, 16));
            CHECK(c1->src[0] == 0xFC && c1->src[1] == 0xFD);   // the fixed block
            CHECK_EQ(c1->c.bufferOffset - c0->c.bufferOffset, 256); CHECK_EQ(c2->c.bufferOffset - c1->c.bufferOffset, 64); CHECK_EQ(c0->c.bufferOffset % 16, 0);
            CHECK_EQ(c0->c.imageExtent.width, 16); CHECK_EQ(c1->c.imageExtent.width, 8); CHECK_EQ(c2->c.imageExtent.height, 4);
            CHECK_EQ(c1->c.imageSubresource.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
        } else CHECK(0);
        CHECK_EQ(A->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // an update with BC rows cannot go into the ASTC image
        vkn_tex_update_stats us0, us1; vkn_tex_update_stats_get(&us0);
        uint8_t row[64]; memset(row, 1, sizeof row);
        CHECK_EQ(vkn_tex_update(A, 0, 0, 0, 0, 16, 4, row, 64), -1);
        vkn_tex_update_stats_get(&us1); CHECK_EQ(us1.dropped_fmt, us0.dropped_fmt + 1);
    }
    // the sRGB twin entry: ASTC 4x4 sRGB over the same blocks
    vkn_tex *A2 = tp_create(H(301, 1), 0x48, 16, 16, 3, 1, sa, na, 0x3A1, ~0u, &why);
    CHECK(A2 != NULL); if (A2) { CHECK_EQ(A2->format, VK_FORMAT_ASTC_4x4_SRGB_BLOCK); CHECK_EQ(A2->astc, 4); }
    // the same shape, other bytes (the entry with its key has another byte count): key miss -> decoded
    vkn_tex *B = tp_create(H(302, 1), 0x44, 16, 16, 3, 1, sb, nb, 0x3A2, ~0u, &why);
    CHECK(B != NULL); if (B) { CHECK_EQ(B->decoded, 1); CHECK_EQ(B->astc, 0); CHECK_EQ(B->format, VK_FORMAT_R8G8B8A8_UNORM); }
    // A's bytes with 2 levels: the level count is part of the key (and of the layout): shape miss, never hashed
    const int kc0 = g_key_calls;
    uint64_t na2; uint8_t *sa2 = tp_storage(0x44, 16, 16, 2, 1, 3, &na2);
    vkn_tex *A3 = sa2 ? tp_create(H(303, 1), 0x44, 16, 16, 2, 1, sa2, na2, 0x3A3, ~0u, &why) : NULL;
    CHECK(A3 != NULL); if (A3) CHECK_EQ(A3->decoded, 1);
    CHECK_EQ(g_key_calls, kc0);
    free(sa2);
    // a 5x5 entry on a device without 5x5: decoded
    vkn_tex *C = tp_create(H(304, 1), 0x42, 20, 20, 1, 1, sc, nc, 0x3A4, ~0u, &why);
    CHECK(C != NULL); if (C) CHECK_EQ(C->decoded, 1);
    // a 6x6 entry, two levels: 12x12 = 2 x 2 blocks, 6x6 = one block
    vkn_tex *D = tp_create(H(305, 1), 0x42, 12, 12, 2, 1, sd, nd, 0x3A5, ~0u, &why);
    CHECK(D != NULL);
    if (D) {
        CHECK_EQ(D->format, VK_FORMAT_ASTC_6x6_UNORM_BLOCK); CHECK_EQ(D->astc, 6);
        flush_logged();
        const mk_copy_t *c0 = copy_of(D->image, 0, 0, 0), *c1 = copy_of(D->image, 1, 0, 0);
        CHECK(c0 && c1 && copies_of(D->image) == 2);
        if (c0 && c1 && c0->src && c1->src) {
            CHECK(!memcmp(c0->src, dd, 64)); CHECK(!memcmp(c1->src, dd + 64, 16)); CHECK_EQ(c1->c.bufferOffset - c0->c.bufferOffset, 64);
            CHECK_EQ(c0->c.imageExtent.width, 12); CHECK_EQ(c1->c.imageExtent.width, 6); CHECK_EQ(c1->c.imageExtent.height, 6);
        } else CHECK(0);
    }
    // not looked up: linear data, BC4, a cube
    { static uint8_t lin16[16 * 16];   // BC3 16x16: 16 blocks
      vkn_tex_region r = {0, 0, 0, 16, 16, 1, lin16};
      vkn_tex_desc d = tdesc(55, 1, 16, 16, 1, 1, VKN_TC_SAMPLED, 0x3A6, ~0u); vkn_tex_src s = {VKN_SRC_REGIONS, &r, 1, NULL, 0, 0, 0};
      vkn_tex *t = vkn_tex_create(H(306, 1), &d, &s, &why); CHECK(t && t->decoded); }
    uint64_t n4; uint8_t *s4 = tp_storage(0x49, 16, 16, 1, 1, 2, &n4);
    vkn_tex *T4 = s4 ? tp_create(H(307, 1), 0x49, 16, 16, 1, 1, s4, n4, 0x3A7, ~0u, &why) : NULL;
    CHECK(T4 && T4->decoded && T4->format == VK_FORMAT_R8_UNORM);
    free(s4);
    { uint8_t *big = (uint8_t *)calloc(1, 1 << 16);
      vkn_tex *Q = big ? tp_create(H(308, 1), 0x44, 16, 16, 1, 8, big, 1 << 16, 0x3A8, ~0u, &why) : NULL;
      CHECK(Q && Q->decoded && Q->layers == 6); free(big); }
    vkn_astc_get_stats(&as);
    CHECK_EQ(as.hits, 3); CHECK_EQ(as.miss_key, 1); CHECK_EQ(as.miss_shape, 1); CHECK_EQ(as.miss_block, 1); CHECK_EQ(as.lookups, 6);
    vkn_tex_stats s1; vkn_tex_get_stats(&s1);
    CHECK_EQ(s1.bc_made[VKN_BC_PACK] - s0.bc_made[VKN_BC_PACK], 3); CHECK_EQ(s1.bc_made[VKN_BC_DECODED] - s0.bc_made[VKN_BC_DECODED], 6);
    CHECK_EQ(s1.bc_alive[VKN_BC_PACK] - s0.bc_alive[VKN_BC_PACK], 3);
    CHECK_EQ(s1.bc_bytes[VKN_BC_PACK] - s0.bc_bytes[VKN_BC_PACK], (A ? A->bytes : 0) + (A2 ? A2->bytes : 0) + (D ? D->bytes : 0));
    CHECK_EQ(s1.dec_why[VKN_DW_MISS_KEY] - s0.dec_why[VKN_DW_MISS_KEY], 1); CHECK_EQ(s1.dec_why[VKN_DW_MISS_SHAPE] - s0.dec_why[VKN_DW_MISS_SHAPE], 1);
    CHECK_EQ(s1.dec_why[VKN_DW_BLOCK] - s0.dec_why[VKN_DW_BLOCK], 1); CHECK_EQ(s1.dec_why[VKN_DW_SRC] - s0.dec_why[VKN_DW_SRC], 1);
    CHECK_EQ(s1.dec_why[VKN_DW_FMT] - s0.dec_why[VKN_DW_FMT], 1); CHECK_EQ(s1.dec_why[VKN_DW_SHAPE] - s0.dec_why[VKN_DW_SHAPE], 1);

    // mip reduction (0x920460): the smaller texture comes CPU-decoded (no data, the old one's NVN format) under a temporary handle; the
    // swap re-makes it as an ASTC 4x4 image and the GPU copies the old levels 1, 2 into it
    if (A) {
        vkn_tex_desc dn = tdesc(55, 1, 8, 8, 2, 1, VKN_TC_SAMPLED, 0x3B0, 3001); dn.nvn_fmt = 0x44;
        vkn_tex *N = vkn_tex_create(H(310, 1), &dn, NULL, &why);
        CHECK(N && N->decoded);
        flush_logged(); mk_nicopy = 0;
        const VkImage nimg = N ? N->image : VK_NULL_HANDLE;
        CHECK_EQ(vkn_tex_swap_reduced(H(300, 1), 0x3A0, 0x3B0), 0);
        vkn_tex *R = vkn_tex_get(H(300, 1));
        CHECK(R != NULL && R != A && R != N); CHECK(vkn_tex_get(H(310, 1)) == NULL);
        if (R) {
            CHECK_EQ(R->astc, 4); CHECK_EQ(R->format, VK_FORMAT_ASTC_4x4_UNORM_BLOCK); CHECK_EQ(R->mips, 2); CHECK_EQ(R->w, 8); CHECK_EQ(R->cookie, 0x3B0);
            CHECK(vkn_tex_by_cookie(0x3B0) == R); CHECK(vkn_tex_view_by_id(3001, NULL) == R->view);
            flush_logged();
            CHECK_EQ(mk_nicopy, 1);
            if (mk_nicopy == 1) {
                const mk_icopy_t *c = &mk_icopy[0];
                CHECK(c->src == A->image && c->dst == R->image && c->dst != nimg); CHECK_EQ(c->n, 2);
                CHECK_EQ(c->c[0].srcSubresource.mipLevel, 1); CHECK_EQ(c->c[0].dstSubresource.mipLevel, 0); CHECK_EQ(c->c[0].extent.width, 8);
                CHECK_EQ(c->c[1].srcSubresource.mipLevel, 2); CHECK_EQ(c->c[1].dstSubresource.mipLevel, 1); CHECK_EQ(c->c[1].extent.height, 4);
                mk_obj *si = mk_obj_of((uint64_t)(uintptr_t)c->src), *di = mk_obj_of((uint64_t)(uintptr_t)c->dst);
                CHECK(si && di && si->ici.format == di->ici.format);
            }
        }
        vkn_tex_get_stats(&s1); CHECK_EQ(s1.pack_tail - s0.pack_tail, 1);
        vkn_tex_destroy_cookie(0x3B0);
    }
    vkn_tex_destroy_cookie(0x3A0); vkn_tex_destroy_cookie(0x3A1); vkn_tex_destroy_cookie(0x3A2); vkn_tex_destroy_cookie(0x3A3); vkn_tex_destroy_cookie(0x3A4);
    vkn_tex_destroy_cookie(0x3A5); vkn_tex_destroy_cookie(0x3A6); vkn_tex_destroy_cookie(0x3A7); vkn_tex_destroy_cookie(0x3A8);
    vkn_tex_get_stats(&s1);
    for (int k = 0; k < VKN_BC_N; k++) { CHECK_EQ(s1.bc_alive[k], s0.bc_alive[k]); CHECK_EQ(s1.bc_bytes[k], s0.bc_bytes[k]); }

    // a device that samples BC: the pack is never asked
    mk_bc = 1;
    vkn_astc_get_stats(&as); const uint64_t lk = as.lookups;
    vkn_tex *E = tp_create(H(320, 1), 0x44, 16, 16, 3, 1, sa, na, 0x3C0, ~0u, &why);
    CHECK(E && E->format == VK_FORMAT_BC3_UNORM_BLOCK && !E->astc); vkn_astc_get_stats(&as); CHECK_EQ(as.lookups, lk);
    vkn_tex_destroy_cookie(0x3C0);
    mk_bc = 0;

    // no pack / an unusable pack / switched off: every BC texture decoded as before (reason "no pack")
    struct { const char *path, *magic, *sw; int bad_size, want; } cases[] = {
        {"/tmp/vkn_stage2_no_such.pack", NULL, NULL, 0, VKN_ASTC_NONE}, {path, "D3AC", NULL, 1, VKN_ASTC_BAD}, {path, "XXXX", NULL, 0, VKN_ASTC_BAD},
        {path, "D3AC", "0", 0, VKN_ASTC_NONE}, {path, "D3AC", NULL, 0, VKN_ASTC_READY}};
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        if (cases[i].magic) {
            tp_ent e2[2] = {tp_of(sa, na, 0x44, 16, 16, 3, 4, da, cases[i].bad_size ? 320 : 336), tp_of(sd, nd, 0x42, 12, 12, 2, 6, dd, 80)};
            tp_write(path, e2, 2, cases[i].magic);
        }
        setenv("DM_ASTC_CACHE_PATH", cases[i].path, 1);
        if (cases[i].sw) setenv("DM_VK_NATIVE_ASTC", cases[i].sw, 1); else unsetenv("DM_VK_NATIVE_ASTC");
        vkn_astc_reset();
        vkn_tex_get_stats(&s0);
        vkn_tex *t = tp_create(H(330 + i, 1), 0x44, 16, 16, 3, 1, sa, na, 0x3D0 + i, ~0u, &why);
        CHECK(t != NULL);
        CHECK_EQ(vkn_astc_open(), cases[i].want);
        vkn_tex_get_stats(&s1);
        if (t) {
            if (cases[i].want == VKN_ASTC_READY) { CHECK_EQ(t->astc, 4); }
            else { CHECK_EQ(t->decoded, 1); CHECK_EQ(s1.dec_why[VKN_DW_OFF] - s0.dec_why[VKN_DW_OFF], 1); }
        }
        vkn_tex_destroy_cookie(0x3D0 + i);
    }
    // a pack truncated after it was opened: the short read becomes transparent black (counted)
    { tp_ent e3[1] = {tp_of(sa, na, 0x44, 16, 16, 3, 4, da, 336)}; tp_write(path, e3, 1, "D3AC");
      setenv("DM_ASTC_CACHE_PATH", path, 1); vkn_astc_reset(); CHECK_EQ(vkn_astc_open(), VKN_ASTC_READY);
      CHECK_EQ(truncate(path, 64 + 100), 0);
      vkn_tex *t = tp_create(H(340, 1), 0x44, 16, 16, 3, 1, sa, na, 0x3E0, ~0u, &why);
      vkn_astc_get_stats(&as); CHECK_EQ(as.read_fails, 1); CHECK(t && t->astc == 4);
      flush_logged(); const mk_copy_t *c = t ? copy_of(t->image, 1, 0, 0) : NULL;
      CHECK(c && c->src && c->src[0] == 0xFC && c->src[1] == 0xFD);   // level 1 lies past the 100 bytes read: void extent
      c = t ? copy_of(t->image, 0, 0, 0) : NULL; CHECK(c && c->src && !memcmp(c->src, da, 100));
      vkn_tex_destroy_cookie(0x3E0); }
    unsetenv("DM_ASTC_CACHE_PATH"); unsetenv("DM_VK_NATIVE_ASTC");
    vkn_astc_reset(); remove(path);
    mk_bc = 1; mk_astc = 7;
    free(sa); free(sb); free(sc); free(sd);
    flush_logged(); frames(3);
}

static void test_tex_targets(void) {
    // colour target: attachment usage and view, no upload queued
    const uint64_t pend0 = vkn_upload_pending_bytes();
    vkn_tex_desc d = tdesc(4, 1, 64, 32, 1, 1, VKN_TC_COLOR, 0xF1, 1000);
    vkn_tex *t = vkn_tex_create(H(50, 1), &d, NULL, NULL);
    CHECK(t != NULL);
    if (t) {
        mk_obj *im = img_of(t);
        CHECK(im && im->ici.usage == (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        mk_obj *a = view_of(t->att);
        CHECK(a && a->vci.viewType == VK_IMAGE_VIEW_TYPE_2D && a->vci.subresourceRange.levelCount == 1 && a->vci.subresourceRange.layerCount == 1 &&
              a->vci.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
        CHECK(t->view != VK_NULL_HANDLE && t->view != t->att); CHECK(vkn_tex_view_by_id(1000, NULL) == t->view);
        CHECK_EQ(vkn_upload_pending_bytes(), pend0);
        vkn_tex_stats ts; vkn_tex_get_stats(&ts); CHECK(ts.alive[VKN_TC_COLOR] == 1 && ts.bytes[VKN_TC_COLOR] == t->bytes);
        vkn_tex_destroy_cookie(0xF1);
        vkn_tex_get_stats(&ts); CHECK(ts.alive[VKN_TC_COLOR] == 0 && ts.bytes[VKN_TC_COLOR] == 0);
    }
    // storage usage when asked for and supported
    d = tdesc(4, 1, 16, 16, 1, 1, VKN_TC_COLOR, 0xF2, ~0u); d.storage = 1;
    t = vkn_tex_create(H(51, 1), &d, NULL, NULL); CHECK(t != NULL); if (t) { mk_obj *im = img_of(t); CHECK(im && (im->ici.usage & VK_IMAGE_USAGE_STORAGE_BIT)); }
    vkn_tex_destroy_cookie(0xF2);
    // window texture (BGRA8)
    d = tdesc(61, 1, 64, 32, 1, 1, VKN_TC_WINDOW, 0xF3, ~0u);
    t = vkn_tex_create(H(52, 1), &d, NULL, NULL);
    CHECK(t != NULL); if (t) { CHECK_EQ(t->format, VK_FORMAT_B8G8R8A8_UNORM); mk_obj *im = img_of(t); CHECK(im && (im->ici.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)); CHECK(t->att != VK_NULL_HANDLE); }
    CHECK_EQ(vkn_tex_destroy_handle(H(52, 1)), 1); CHECK_EQ(vkn_tex_destroy_handle(H(52, 1)), 0);
    // depth targets: D24S8 = both aspects, the sampled view depth only; D16; without D24 support the D32F stand-ins
    static const struct { uint32_t e; int d24; VkFormat f; VkImageAspectFlags asp; } dt[] = {
        {51, 1, VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT}, {50, 1, VK_FORMAT_X8_D24_UNORM_PACK32, VK_IMAGE_ASPECT_DEPTH_BIT},
        {49, 1, VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT}, {52, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT},
        {51, 0, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT}, {50, 0, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT}};
    for (unsigned i = 0; i < sizeof dt / sizeof *dt; i++) {
        mk_d24 = dt[i].d24;
        d = tdesc(dt[i].e, 1, 32, 32, 1, 1, VKN_TC_DEPTH, 0xF10 + i, ~0u);
        t = vkn_tex_create(H(60 + i, 1), &d, NULL, NULL);
        CHECK(t != NULL); if (!t) continue;
        CHECK_EQ(t->format, dt[i].f); CHECK_EQ(t->aspect, dt[i].asp);
        mk_obj *im = img_of(t);
        CHECK(im && im->ici.usage == (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
        mk_obj *v = view_of(t->view), *a = view_of(t->att);
        CHECK(v && v->vci.subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT); CHECK(a && a->vci.subresourceRange.aspectMask == dt[i].asp);
        vkn_tex_destroy_cookie(0xF10 + i);
    }
    mk_d24 = 1;
    // a colour target in a format that cannot be rendered: refused (BC; an RGB8 target is not widened)
    const char *why = NULL;
    d = tdesc(53, 1, 16, 16, 1, 1, VKN_TC_COLOR, 0xF20, ~0u);
    CHECK(vkn_tex_create(H(70, 1), &d, NULL, &why) == NULL); CHECK(why && !strcmp(why, "colour format not renderable"));
    d = tdesc(3, 1, 16, 16, 1, 1, VKN_TC_COLOR, 0xF21, ~0u);
    CHECK(vkn_tex_create(H(71, 1), &d, NULL, &why) == NULL);
    // multisampled: samples kept, one level
    d = tdesc(4, 5, 32, 32, 3, 1, VKN_TC_COLOR, 0xF22, ~0u); d.samples = 4;
    t = vkn_tex_create(H(72, 1), &d, NULL, NULL);
    CHECK(t != NULL); if (t) { mk_obj *im = img_of(t); CHECK(im && im->ici.samples == VK_SAMPLE_COUNT_4_BIT && im->ici.mipLevels == 1); }
    vkn_tex_destroy_cookie(0xF22);
    flush_logged(); frames(3);
}

// [VK-DRV] D24 stand-ins of any class and the depth plane of uploads (vkn_tex.h; docs/dos2/nr/VK-DRIVERS.md)
static void test_tex_depth_standin(void) {
    // the plane: NVN Z24S8 / X8Z24 depth in bits 8..31 -> Vulkan D24 bits 0..23 / a float for the D32F stand-in; D16 / D32F as is
    {const uint32_t src[3] = {0x80000012u, 0xFFFFFF00u, 0x000000ABu}; uint32_t o[3]; float f[3];
     CHECK_EQ(vkn_tex_depth_plane(0x35, 0, (const uint8_t *)src, 3, (uint8_t *)o), 4); CHECK(o[0] == 0x800000u && o[1] == 0xFFFFFFu && o[2] == 0);
     CHECK_EQ(vkn_tex_depth_plane(0x33, 1, (const uint8_t *)src, 3, (uint8_t *)f), 4);
     CHECK(fabsf(f[0] - 8388608.0f / 16777215.0f) < 1e-7f && f[1] == 1.0f && f[2] == 0.0f);
     const uint16_t d16[2] = {7, 65535}; uint16_t o16[2]; CHECK_EQ(vkn_tex_depth_plane(0x32, 0, (const uint8_t *)d16, 2, (uint8_t *)o16), 2); CHECK(o16[0] == 7 && o16[1] == 65535);
     const uint32_t z32s8[4] = {0x3f000000u, 0x55u, 0x3f800000u, 0x66u}; uint32_t o32[2];
     CHECK_EQ(vkn_tex_depth_plane(0x36, 0, (const uint8_t *)z32s8, 2, (uint8_t *)o32), 4); CHECK(o32[0] == 0x3f000000u && o32[1] == 0x3f800000u);
     CHECK_EQ(vkn_tex_depth_plane(0x31, 0, (const uint8_t *)src, 1, (uint8_t *)o), 0); CHECK_EQ(vkn_tex_depth_plane(4, 0, (const uint8_t *)src, 1, (uint8_t *)o), 0);}
    // the SIM tokens of the no-D24 model (xbg_sim_parse XBG_SIM_NOD24: the xclipse profile, nod24)
    CHECK(vkn_policy_sim_nod24("xclipse") && vkn_policy_sim_nod24("mali,nod24") && !vkn_policy_sim_nod24("mali") && !vkn_policy_sim_nod24(NULL) && !vkn_policy_sim_nod24("xclipsex"));
    // a sampled Z24S8 texture with data: on a device with D24 the depth plane is staged (bits 0..23), without D24 a D32F + S8 stand-in
    // with the depth as floats (before VK-DRV: the raw words / "format not sampled by the device")
    uint32_t px[4 * 4]; for (unsigned i = 0; i < 16; i++) px[i] = ((i * 0x111111u) & 0xFFFFFFu) << 8 | 0x5Au;
    for (int d24 = 1; d24 >= 0; d24--) {
        mk_d24 = d24;
        vkn_tex_region rg = {0, 0, 0, 4, 4, 1, px}; vkn_tex_src src = {VKN_SRC_REGIONS, &rg, 1, NULL, 0, 0, 0};
        vkn_tex_desc d = tdesc(51, 1, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xD24 + d24, ~0u);
        const char *why = NULL;
        vkn_tex *t = vkn_tex_create(H(90 + d24, 1), &d, &src, &why);
        CHECK(t != NULL); if (!t) { printf("  (why: %s)\n", why ? why : "-"); continue; }
        CHECK_EQ(t->format, d24 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT); CHECK_EQ(t->d24f, d24 ? 0 : 1);
        flush_logged();
        const mk_copy_t *c = copy_of(t->image, 0, 0, 0);
        CHECK(c && c->src && c->c.imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
        if (c && c->src) for (unsigned i = 0; i < 16; i++) {
            uint32_t w; memcpy(&w, c->src + 4 * i, 4); float f; memcpy(&f, &w, 4);
            const uint32_t dep = px[i] >> 8;
            if (d24) CHECK_EQ(w, dep); else CHECK(fabsf(f - (float)dep / 16777215.0f) < 1e-7f);
        }
        vkn_tex_destroy_cookie(0xD24 + d24);
    }
    mk_d24 = 1;
    uint64_t made = 0, conv = 0; int on = 0; vkn_tex_depth_stats(&made, &conv, &on);
    CHECK(made >= 1 && conv >= 16 && on == 1);
    flush_logged(); frames(3);
}

static void test_tex_lifetime(void) {
    // a destroy before the upload ran: the upload records nothing, the image lives until the upload dropped its reference
    uint8_t px[4 * 4 * 4]; memset(px, 7, sizeof px);
    vkn_tex_region r = {0, 0, 0, 4, 4, 1, px}; vkn_tex_src s = {VKN_SRC_REGIONS, &r, 1, NULL, 0, 0, 0};
    vkn_tex_desc d = tdesc(4, 1, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xA0, 1100);
    vkn_tex *t = vkn_tex_create(H(80, 1), &d, &s, NULL);
    CHECK(t != NULL); if (!t) return;
    const VkImage img = t->image;
    CHECK_EQ(vkn_tex_destroy_cookie(0xA0), 1);
    frames(4); CHECK(mk_is_live((uint64_t)(uintptr_t)img, MK_IMAGE));   // the queued upload still holds it
    flush_logged(); CHECK_EQ(copies_of(img), 0); CHECK_EQ(bars_of(img), 0);
    frames(2); CHECK(mk_is_live((uint64_t)(uintptr_t)img, MK_IMAGE));
    frames(1); CHECK(!mk_is_live((uint64_t)(uintptr_t)img, MK_IMAGE));
    // a new texture under the same handle index: the old one stays known by its cookie (and keeps its pool id) until its destroy
    vkn_tex_desc da = tdesc(4, 1, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xA1, 1200), db = tdesc(4, 1, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xB1, 1201);
    vkn_tex *A = vkn_tex_create(H(81, 1), &da, NULL, NULL), *B = vkn_tex_create(H(81, 2), &db, NULL, NULL);
    CHECK(A && B);
    if (A && B) {
        CHECK(vkn_tex_get(H(81, 2)) == B); CHECK(vkn_tex_get(H(81, 1)) == NULL); CHECK(vkn_tex_by_cookie(0xA1) == A);
        CHECK(vkn_tex_view_by_id(1200, NULL) == A->view); CHECK(vkn_tex_view_by_id(1201, NULL) == B->view);
        vkn_tex_stats ts; vkn_tex_get_stats(&ts); CHECK_EQ(ts.pending, 1);
        CHECK_EQ(vkn_tex_destroy_handle(H(81, 1)), 0);
        CHECK_EQ(vkn_tex_destroy_cookie(0xA1), 1); vkn_tex_get_stats(&ts); CHECK_EQ(ts.pending, 0);
        CHECK(vkn_tex_get(H(81, 2)) == B); CHECK(vkn_tex_view_by_id(1200, NULL) == VK_NULL_HANDLE);
        // the same cookie again without a destroy: the engine reused its object, the old texture is gone
        vkn_tex_desc dc = tdesc(4, 1, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xB1, 1201);
        vkn_tex *C = vkn_tex_create(H(82, 1), &dc, NULL, NULL);
        CHECK(C != NULL); CHECK(vkn_tex_by_cookie(0xB1) == C); CHECK(vkn_tex_get(H(81, 2)) == NULL); CHECK(vkn_tex_view_by_id(1201, NULL) == (C ? C->view : NULL));
        // a pool id taken over by a newer texture is not cleared by the older one's destroy
        vkn_tex_desc de = tdesc(4, 1, 4, 4, 1, 1, VKN_TC_SAMPLED, 0xB2, 1201);
        vkn_tex *E = vkn_tex_create(H(83, 1), &de, NULL, NULL);
        CHECK(E && vkn_tex_view_by_id(1201, NULL) == E->view);
        vkn_tex_destroy_cookie(0xB1); CHECK(E && vkn_tex_view_by_id(1201, NULL) == E->view);
        vkn_tex_destroy_cookie(0xB2);
    }
    flush_logged(); frames(3);
    // mip reduction (0x920460 / 0x920b00): old 16x16 x 4 levels, new 4x4 x 2 levels under a temporary handle; the handle takes the new
    // texture, the GPU copies old levels 2, 3 into new levels 0, 1
    vkn_tex_desc dO = tdesc(4, 1, 16, 16, 4, 1, VKN_TC_SAMPLED, 0x0D, 1300), dN = tdesc(4, 1, 4, 4, 2, 1, VKN_TC_SAMPLED, 0x0E, 1301);
    vkn_tex *O = vkn_tex_create(H(90, 1), &dO, NULL, NULL), *N = vkn_tex_create(H(91, 1), &dN, NULL, NULL);
    CHECK(O && N);
    if (O && N) {
        flush_logged();
        mk_nicopy = 0;
        vkn_tex_stats ts; vkn_tex_get_stats(&ts); const uint64_t sw0 = ts.swaps;
        CHECK_EQ(vkn_tex_swap_reduced(H(90, 1), 0x0D, 0x0E), 0);
        CHECK(vkn_tex_get(H(90, 1)) == N); CHECK(vkn_tex_get(H(91, 1)) == NULL); CHECK(vkn_tex_by_cookie(0x0D) == O);
        vkn_tex_get_stats(&ts); CHECK_EQ(ts.swaps, sw0 + 1);
        flush_logged();
        CHECK_EQ(mk_nicopy, 1);
        if (mk_nicopy == 1) {
            const mk_icopy_t *c = &mk_icopy[0];
            CHECK(c->src == O->image && c->dst == N->image); CHECK_EQ(c->n, 2);
            CHECK_EQ(c->sl, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); CHECK_EQ(c->dl, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            CHECK_EQ(c->c[0].srcSubresource.mipLevel, 2); CHECK_EQ(c->c[0].dstSubresource.mipLevel, 0); CHECK_EQ(c->c[0].extent.width, 4); CHECK_EQ(c->c[0].extent.height, 4);
            CHECK_EQ(c->c[1].srcSubresource.mipLevel, 3); CHECK_EQ(c->c[1].dstSubresource.mipLevel, 1); CHECK_EQ(c->c[1].extent.width, 2); CHECK_EQ(c->c[1].extent.depth, 1);
            CHECK_EQ(c->c[0].srcSubresource.layerCount, 1);
        }
        CHECK_EQ(O->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); CHECK_EQ(N->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // another format: no copy (-2); an unknown new cookie: -1
        vkn_tex_desc dP = tdesc(2, 1, 4, 4, 2, 1, VKN_TC_SAMPLED, 0x0F, ~0u);
        vkn_tex *P = vkn_tex_create(H(92, 1), &dP, NULL, NULL);
        CHECK(P != NULL); CHECK_EQ(vkn_tex_swap_reduced(H(90, 1), 0x0D, 0x0F), -2);
        CHECK_EQ(vkn_tex_swap_reduced(H(90, 1), 0x0D, 0x777), -1);
        vkn_tex_destroy_cookie(0x0D); vkn_tex_destroy_cookie(0x0E); vkn_tex_destroy_cookie(0x0F);
    }
    flush_logged(); frames(3);
}

// Shared render-target storage (vkn_tex_create_shared; 0x914bb0 reservations, flag 0x2000 of 0x91e2a0): textures of one storage
// identity (pool block, offset) and one shape are ONE vkn_tex (one image, layout, pass state; every name resolves to it), the image
// lives until the last name goes (both destroy orders, by cookie and by handle), a different size / format / data gets an image of its
// own (counted by reason), another identity or a plain create never attaches, and a resize generation on the same storage (0x914bb0 is
// never called again: the new targets land on the same reservation) attaches only to a live image of its own shape.
static vkn_tex *sh_create(uint64_t h, uint32_t efmt, uint32_t w, uint32_t hh, uint64_t cookie, uint32_t pool_id, uint64_t blk, uint64_t off, const char *name) {
    vkn_tex_desc d = tdesc(efmt, 1, w, hh, 1, 1, VKN_TC_COLOR, cookie, pool_id);
    const vkn_tex_share sh = {blk, off, (uint64_t)w * hh * 4, name, "Key"};
    return vkn_tex_create_shared(h, &d, NULL, &sh, NULL);
}
static void test_tex_shared(void) {
    flush_logged(); frames(3);
    vkn_tex_stats s0, ts; vkn_tex_get_stats(&s0);
    const uint64_t img0 = mk_live(MK_IMAGE);
    // (1) same identity, same shape: one object; every lookup of B resolves to A; pass state is one
    vkn_tex *A = sh_create(H(400, 1), 4, 96, 48, 0x5A0, 4000, 0x9000, 0x100, "HalfWidthBackBuffer");
    vkn_tex *B = sh_create(H(401, 1), 4, 96, 48, 0x5B0, 4001, 0x9000, 0x100, "ScreenshotBackBuffer");
    CHECK(A != NULL); CHECK(B == A); CHECK_EQ(mk_live(MK_IMAGE), img0 + 1);
    if (!A || B != A) return;
    CHECK(vkn_tex_get(H(400, 1)) == A); CHECK(vkn_tex_get(H(401, 1)) == A);
    CHECK(vkn_tex_by_cookie(0x5A0) == A); CHECK(vkn_tex_by_cookie(0x5B0) == A);
    vkn_tex *vt = NULL;
    CHECK(vkn_tex_view_by_id(4000, &vt) == A->view && vt == A); vt = NULL; CHECK(vkn_tex_view_by_id(4001, &vt) == A->view && vt == A);
    CHECK_EQ(A->handle, H(400, 1)); CHECK_EQ(A->cookie, 0x5A0); CHECK_EQ(A->nnames, 1);
    A->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; A->pass_ever = 1;   // (what vkn_pass writes through either name)
    CHECK_EQ(vkn_tex_by_cookie(0x5B0)->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); CHECK_EQ(vkn_tex_get(H(401, 1))->pass_ever, 1);
    CHECK(vkn_tex_att_view(vkn_tex_by_cookie(0x5B0), 0, 0) == A->att);
    vkn_tex_get_stats(&ts);
    CHECK_EQ(ts.sh_storages, s0.sh_storages + 1); CHECK_EQ(ts.sh_images, s0.sh_images + 1); CHECK_EQ(ts.sh_aliases, s0.sh_aliases + 1);
    CHECK_EQ(ts.sh_made, s0.sh_made + 1); CHECK_EQ(ts.sh_attached, s0.sh_attached + 1);
    CHECK_EQ(ts.alive[VKN_TC_COLOR], s0.alive[VKN_TC_COLOR] + 1); CHECK_EQ(ts.bytes[VKN_TC_COLOR], s0.bytes[VKN_TC_COLOR] + A->bytes); CHECK_EQ(ts.pending, s0.pending);
    // (2) the owner's own name goes first: the image stays, B takes over the object's identity; then B goes: the image is retired
    const VkImage ia = A->image;
    CHECK_EQ(vkn_tex_destroy_cookie(0x5A0), 1);
    CHECK(!A->dead); CHECK(vkn_tex_get(H(400, 1)) == NULL); CHECK(vkn_tex_by_cookie(0x5A0) == NULL); CHECK(vkn_tex_view_by_id(4000, NULL) == VK_NULL_HANDLE);
    CHECK(vkn_tex_get(H(401, 1)) == A); CHECK(vkn_tex_by_cookie(0x5B0) == A); CHECK(vkn_tex_view_by_id(4001, NULL) == A->view);
    CHECK_EQ(A->handle, H(401, 1)); CHECK_EQ(A->cookie, 0x5B0); CHECK_EQ(A->pool_id, 4001); CHECK_EQ(A->nnames, 0); CHECK(!strcmp(A->sname, "ScreenshotBackBuffer"));
    CHECK_EQ(A->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);   // (state untouched by the hand-over)
    frames(4); CHECK(mk_is_live((uint64_t)(uintptr_t)ia, MK_IMAGE));
    vkn_tex_get_stats(&ts); CHECK_EQ(ts.sh_handover, s0.sh_handover + 1); CHECK_EQ(ts.sh_aliases, s0.sh_aliases); CHECK_EQ(ts.alive[VKN_TC_COLOR], s0.alive[VKN_TC_COLOR] + 1);
    CHECK_EQ(vkn_tex_destroy_cookie(0x5A0), 0);
    CHECK_EQ(vkn_tex_destroy_cookie(0x5B0), 1);
    CHECK(vkn_tex_get(H(401, 1)) == NULL); CHECK(vkn_tex_view_by_id(4001, NULL) == VK_NULL_HANDLE);
    frames(2); CHECK(mk_is_live((uint64_t)(uintptr_t)ia, MK_IMAGE)); frames(1); CHECK(!mk_is_live((uint64_t)(uintptr_t)ia, MK_IMAGE));
    vkn_tex_get_stats(&ts); CHECK_EQ(ts.sh_images, s0.sh_images); CHECK_EQ(ts.sh_storages, s0.sh_storages); CHECK_EQ(ts.alive[VKN_TC_COLOR], s0.alive[VKN_TC_COLOR]);
    // (3) the other order: the attached name goes first (by handle), then the owner
    A = sh_create(H(402, 1), 4, 96, 48, 0x5C0, 4002, 0x9000, 0x100, "A");
    B = sh_create(H(403, 1), 4, 96, 48, 0x5D0, 4003, 0x9000, 0x100, "B");
    CHECK(A && B == A);
    if (A && B == A) {
        const VkImage ib = A->image;
        CHECK_EQ(vkn_tex_destroy_handle(H(403, 1)), 1);
        CHECK(vkn_tex_get(H(403, 1)) == NULL); CHECK(vkn_tex_by_cookie(0x5D0) == NULL); CHECK(vkn_tex_view_by_id(4003, NULL) == VK_NULL_HANDLE);
        CHECK(vkn_tex_get(H(402, 1)) == A); CHECK(vkn_tex_view_by_id(4002, NULL) == A->view); CHECK_EQ(A->handle, H(402, 1)); CHECK(!A->dead);
        frames(4); CHECK(mk_is_live((uint64_t)(uintptr_t)ib, MK_IMAGE));
        CHECK_EQ(vkn_tex_destroy_cookie(0x5D0), 0);
        CHECK_EQ(vkn_tex_destroy_cookie(0x5C0), 1);
        frames(3); CHECK(!mk_is_live((uint64_t)(uintptr_t)ib, MK_IMAGE));
    }
    // (4) three names, the middle one first; a newer texture takes an attached name's handle index (that name keeps its cookie)
    A = sh_create(H(404, 1), 4, 96, 48, 0x5E0, 4004, 0x9000, 0x100, "A");
    B = sh_create(H(405, 1), 4, 96, 48, 0x5E1, 4005, 0x9000, 0x100, "B");
    vkn_tex *C = sh_create(H(406, 1), 4, 96, 48, 0x5E2, 4006, 0x9000, 0x100, "C");
    CHECK(A && B == A && C == A); CHECK_EQ(A ? A->nnames : 0, 2);
    if (A && B == A && C == A) {
        vkn_tex_desc dx = tdesc(4, 1, 8, 8, 1, 1, VKN_TC_SAMPLED, 0x5EF, ~0u);
        vkn_tex *X = vkn_tex_create(H(406, 2), &dx, NULL, NULL);
        CHECK(X && X != A); CHECK(vkn_tex_get(H(406, 2)) == X); CHECK(vkn_tex_get(H(406, 1)) == NULL); CHECK(vkn_tex_by_cookie(0x5E2) == A);
        vkn_tex_get_stats(&ts); CHECK_EQ(ts.pending, s0.pending);   // (A is still linked under its own handle)
        CHECK_EQ(vkn_tex_destroy_cookie(0x5E1), 1); CHECK(vkn_tex_get(H(405, 1)) == NULL); CHECK(vkn_tex_get(H(404, 1)) == A); CHECK_EQ(A->nnames, 1);
        CHECK_EQ(vkn_tex_destroy_cookie(0x5E0), 1);   // hand-over to C, which has lost its handle index: known by cookie only
        CHECK(vkn_tex_get(H(404, 1)) == NULL); CHECK(vkn_tex_by_cookie(0x5E2) == A); CHECK(!A->linked); CHECK(vkn_tex_get(H(406, 2)) == X);
        vkn_tex_get_stats(&ts); CHECK_EQ(ts.pending, s0.pending + 1);
        CHECK_EQ(vkn_tex_destroy_handle(H(406, 1)), 0);   // (that index belongs to X now)
        CHECK(vkn_tex_get(H(406, 2)) == X);
        CHECK_EQ(vkn_tex_destroy_cookie(0x5E2), 1); CHECK(vkn_tex_get(H(406, 2)) == X);
        CHECK_EQ(vkn_tex_destroy_cookie(0x5EF), 1);
    }
    flush_logged(); frames(3);
    // (5) what differs stays separate, counted by reason: size, format; another identity / a plain create never attaches
    vkn_tex_get_stats(&s0);
    A = sh_create(H(410, 1), 4, 96, 48, 0x600, 4010, 0x9000, 0x200, "A");
    vkn_tex *S = sh_create(H(411, 1), 4, 48, 24, 0x601, 4011, 0x9000, 0x200, "S");      // half size, same storage
    vkn_tex *F = sh_create(H(412, 1), 24, 96, 48, 0x602, 4012, 0x9000, 0x200, "F");     // RGBA32F, same storage
    vkn_tex *O = sh_create(H(413, 1), 4, 96, 48, 0x603, 4013, 0x9000, 0x300, "O");      // another offset
    vkn_tex *Q = sh_create(H(414, 1), 4, 96, 48, 0x604, 4014, 0x9001, 0x200, "Q");      // another block
    vkn_tex_desc dp = tdesc(4, 1, 96, 48, 1, 1, VKN_TC_COLOR, 0x605, 4015);
    vkn_tex *P = vkn_tex_create(H(415, 1), &dp, NULL, NULL);                            // no storage identity
    CHECK(A && S && F && O && Q && P);
    CHECK(S != A && F != A && O != A && Q != A && P != A && F != S);
    vkn_tex *A2 = sh_create(H(416, 1), 4, 96, 48, 0x606, 4016, 0x9000, 0x200, "A2");    // the chain holds A, S, F: A2 finds A
    CHECK(A2 == A);
    vkn_tex *S2 = sh_create(H(417, 1), 4, 48, 24, 0x607, 4017, 0x9000, 0x200, "S2");    // ... and S2 finds S
    CHECK(S2 == S);
    // initial data: never attached (it would land in the other texture's image)
    uint8_t px[96 * 48 * 4]; memset(px, 3, sizeof px);
    vkn_tex_region r = {0, 0, 0, 96, 48, 1, px}; const vkn_tex_src src = {VKN_SRC_REGIONS, &r, 1, NULL, 0, 0, 0};
    vkn_tex_desc dd = tdesc(4, 1, 96, 48, 1, 1, VKN_TC_COLOR, 0x608, 4018);
    const vkn_tex_share shd = {0x9000, 0x200, 0, "D", "Key"};
    vkn_tex *D = vkn_tex_create_shared(H(418, 1), &dd, &src, &shd, NULL);
    CHECK(D && D != A);
    vkn_tex_get_stats(&ts);
    CHECK_EQ(ts.sh_kept[VKN_AL_SIZE], s0.sh_kept[VKN_AL_SIZE] + 1); CHECK_EQ(ts.sh_kept[VKN_AL_FORMAT], s0.sh_kept[VKN_AL_FORMAT] + 1);
    CHECK_EQ(ts.sh_kept[VKN_AL_DATA], s0.sh_kept[VKN_AL_DATA] + 1); CHECK_EQ(ts.sh_attached, s0.sh_attached + 2);
    CHECK_EQ(ts.sh_made, s0.sh_made + 6);   // A, S, F, O, Q, D
    CHECK_EQ(ts.sh_images, s0.sh_images + 6); CHECK_EQ(ts.sh_storages, s0.sh_storages + 3); CHECK_EQ(ts.sh_aliases, s0.sh_aliases + 2);
    CHECK(vkn_tex_view_by_id(4016, NULL) == A->view); CHECK(vkn_tex_view_by_id(4017, NULL) == S->view); CHECK(vkn_tex_view_by_id(4015, NULL) == P->view);
    // the different-size image survives its storage neighbours
    vkn_tex_destroy_cookie(0x600); vkn_tex_destroy_cookie(0x606);
    CHECK(vkn_tex_get(H(411, 1)) == S && !S->dead); CHECK(vkn_tex_by_cookie(0x607) == S);
    for (uint64_t c = 0x601; c <= 0x608; c++) vkn_tex_destroy_cookie(c);
    flush_logged(); frames(3);
    // (6) a resize generation on the same storage (0x912f70 -> new targets, the same reservation): while the old generation lives, a
    // new size is separate; after the old one is gone, the new generation's second name attaches to the new image, not to a dead one
    vkn_tex_get_stats(&s0);
    vkn_tex *G1 = sh_create(H(420, 1), 4, 96, 48, 0x700, 4020, 0x9000, 0x400, "G1"), *G1b = sh_create(H(421, 1), 4, 96, 48, 0x701, 4021, 0x9000, 0x400, "G1b");
    CHECK(G1 && G1b == G1);
    vkn_tex *N1 = sh_create(H(422, 1), 4, 128, 64, 0x702, 4022, 0x9000, 0x400, "N1");
    CHECK(N1 && N1 != G1);
    vkn_tex_destroy_cookie(0x700); vkn_tex_destroy_cookie(0x701);   // the old generation goes (deferred destroys of 0x91d630)
    vkn_tex *N1b = sh_create(H(423, 1), 4, 128, 64, 0x703, 4023, 0x9000, 0x400, "N1b");
    CHECK(N1b == N1);
    vkn_tex *G2 = sh_create(H(424, 1), 4, 96, 48, 0x704, 4024, 0x9000, 0x400, "G2");   // back to the old size: the old image is gone, a new one
    CHECK(G2 && G2 != N1); CHECK(G2 && !G2->dead && G2->nnames == 0);
    vkn_tex_get_stats(&ts);
    CHECK_EQ(ts.sh_made, s0.sh_made + 3); CHECK_EQ(ts.sh_attached, s0.sh_attached + 2); CHECK_EQ(ts.sh_kept[VKN_AL_SIZE], s0.sh_kept[VKN_AL_SIZE] + 2);
    CHECK_EQ(ts.sh_images, s0.sh_images + 2); CHECK_EQ(ts.sh_storages, s0.sh_storages + 1);
    for (uint64_t c = 0x702; c <= 0x704; c++) vkn_tex_destroy_cookie(c);
    // (7) an engine object reused without a destroy event while attached: that name goes, the image keeps its other name
    A = sh_create(H(430, 1), 4, 96, 48, 0x710, 4030, 0x9000, 0x500, "A");
    B = sh_create(H(431, 1), 4, 96, 48, 0x711, 4031, 0x9000, 0x500, "B");
    CHECK(A && B == A);
    vkn_tex_desc dr = tdesc(4, 1, 8, 8, 1, 1, VKN_TC_SAMPLED, 0x711, 4032);
    vkn_tex *R = vkn_tex_create(H(432, 1), &dr, NULL, NULL);
    CHECK(R && R != A); CHECK(vkn_tex_by_cookie(0x711) == R); CHECK(vkn_tex_get(H(431, 1)) == NULL); CHECK(A && A->nnames == 0 && !A->dead);
    vkn_tex_destroy_cookie(0x711); vkn_tex_destroy_cookie(0x710);
    flush_logged(); frames(3);
    vkn_tex_get_stats(&ts);
    CHECK_EQ(ts.sh_images, 0); CHECK_EQ(ts.sh_storages, 0); CHECK_EQ(ts.sh_aliases, 0);
    CHECK_EQ(mk_live(MK_IMAGE), img0);
}

// Samplers: every filter / wrap / compare value (MAP-1 §3.2; out-of-range values as CreateSampler 0x921e40 treats them).
static void test_samplers(void) {
    // filter 0..5 -> (mag, min, mip); NVN min filter numbers of the doc: 1..5 -> 3, 3, 5, 3, 5 (3 linear + nearest level, 5 linear +
    // linear level), mag linear; 0 -> 2 (nearest + nearest level), mag nearest; above 5 like 0 (0x921e40: (filter - 1) > 4 -> min 2, mag 0)
    static const struct { VkFilter mag, min; VkSamplerMipmapMode mip; } kf[7] = {
        {VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST}, {VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST},
        {VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST}, {VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR},
        {VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST}, {VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR},
        {VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST}};
    // wrap: 0 repeat (NVN 1), 1 clamp to edge (7), 2 mirrored repeat (6), 3 clamp to border (5); 4 -> repeat
    static const VkSamplerAddressMode kw[5] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_REPEAT};
    // compare: engine n -> NVN n + 1 = never, less, equal, lequal, greater, notequal, gequal, always; 0 = no compare; 8 -> compare with NVN 1 (never)
    static const VkCompareOp kc[9] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                      VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER};
    static const uint32_t an[4] = {1, 4, 16, 64};
    uint32_t n = 0; static VkSampler made[400];
    const uint64_t s0 = mk_created[MK_SAMPLER];
    for (uint32_t f = 0; f < 7; f++) for (uint32_t w = 0; w < 5; w++) for (uint32_t c = 0; c < 9; c++) {
        const uint32_t id = 10 + n, a = an[(f + w + c) % 4];
        CHECK_EQ(vkn_sampler_create(H(id, 1), id, f, w, (w + 1) % 5, (w + 2) % 5, a, c), 0);
        CHECK_EQ(mk_created[MK_SAMPLER], s0 + n + 1);
        const VkSampler s = (VkSampler)(uintptr_t)mk_last(MK_SAMPLER); made[n++] = s;
        CHECK(vkn_sampler_by_id(id) == s);
        mk_obj *o = mk_obj_of((uint64_t)(uintptr_t)s); if (!o) { CHECK(0); continue; }
        const VkSamplerCreateInfo *ci = &o->sci;
        CHECK_EQ(ci->magFilter, kf[f].mag); CHECK_EQ(ci->minFilter, kf[f].min); CHECK_EQ(ci->mipmapMode, kf[f].mip);
        CHECK_EQ(ci->addressModeU, kw[w]); CHECK_EQ(ci->addressModeV, kw[(w + 1) % 5]); CHECK_EQ(ci->addressModeW, kw[(w + 2) % 5]);
        CHECK_EQ(ci->compareEnable, c != 0); if (c) CHECK_EQ(ci->compareOp, kc[c]);
        CHECK_EQ(ci->borderColor, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);   // "цвет границы нулевой"
        CHECK_EQ(ci->anisotropyEnable, a > 1); CHECK(ci->maxAnisotropy == (a > 16 ? 16.0f : (float)a));
        CHECK_EQ(ci->unnormalizedCoordinates, 0); CHECK(ci->minLod == 0.0f && ci->maxLod >= 1000.0f); CHECK(ci->mipLodBias == 0.0f);
    }
    vkn_tex_stats ts; vkn_tex_get_stats(&ts); CHECK_EQ(ts.samplers, n);
    // destroy: unknown at once, the VkSampler 3 frame ends later
    CHECK_EQ(vkn_sampler_destroy(H(10, 1)), 1); CHECK_EQ(vkn_sampler_destroy(H(10, 1)), 0); CHECK_EQ(vkn_sampler_destroy(H(11, 2)), 0);
    CHECK(vkn_sampler_by_id(10) == VK_NULL_HANDLE);
    frames(2); CHECK(mk_is_live((uint64_t)(uintptr_t)made[0], MK_SAMPLER)); frames(1); CHECK(!mk_is_live((uint64_t)(uintptr_t)made[0], MK_SAMPLER));
    // a create over a live index (a destroy we never saw): the old sampler is retired, its pool id cleared
    CHECK_EQ(vkn_sampler_create(H(11, 2), 3000, 1, 0, 0, 0, 1, 0), 0);
    CHECK(vkn_sampler_by_id(11) == VK_NULL_HANDLE); CHECK(vkn_sampler_by_id(3000) != VK_NULL_HANDLE);
    frames(3); CHECK(!mk_is_live((uint64_t)(uintptr_t)made[1], MK_SAMPLER));
    CHECK_EQ(vkn_sampler_destroy(H(11, 2)), 1);
    for (uint32_t i = 2; i < n; i++) CHECK_EQ(vkn_sampler_destroy(H(10 + i, 1)), 1);
    CHECK(vkn_sampler_by_id(0x1000) == VK_NULL_HANDLE);
    frames(3);
    vkn_tex_get_stats(&ts); CHECK_EQ(ts.samplers, 0);
}

static void test_tex_shapes(void) {
    // cube array: 6 x cubes layers, CUBE_ARRAY view, per-face ids for the first 7 layers only
    vkn_tex_desc d = tdesc(4, 9, 4, 4, 1, 2, VKN_TC_SAMPLED, 0x9A1, ~0u);
    for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) d.layer_ids[i] = 1400 + i;
    vkn_tex *t = vkn_tex_create(H(95, 1), &d, NULL, NULL);
    CHECK(t != NULL);
    if (t) {
        mk_obj *im = img_of(t); CHECK(im && im->ici.arrayLayers == 12 && (im->ici.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT));
        mk_obj *v = view_of(t->view); CHECK(v && v->vci.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && v->vci.subresourceRange.layerCount == 12);
        mk_obj *l6 = view_of(t->layer_view[6]); CHECK(l6 && l6->vci.subresourceRange.baseArrayLayer == 6 && l6->vci.viewType == VK_IMAGE_VIEW_TYPE_2D);
        CHECK(vkn_tex_view_by_id(1406, NULL) == t->layer_view[6]);
        vkn_tex_destroy_cookie(0x9A1);
    }
    // 1D array: height 1, layers from the descriptor, 1D_ARRAY view, 1D layer views
    d = tdesc(4, 3, 16, 7, 1, 4, VKN_TC_SAMPLED, 0x9A2, ~0u); d.layer_ids[1] = 1450;
    t = vkn_tex_create(H(96, 1), &d, NULL, NULL);
    CHECK(t != NULL);
    if (t) {
        mk_obj *im = img_of(t); CHECK(im && im->ici.imageType == VK_IMAGE_TYPE_1D && im->ici.extent.height == 1 && im->ici.arrayLayers == 4);
        mk_obj *v = view_of(t->view); CHECK(v && v->vci.viewType == VK_IMAGE_VIEW_TYPE_1D_ARRAY);
        mk_obj *l1 = view_of(t->layer_view[1]); CHECK(l1 && l1->vci.viewType == VK_IMAGE_VIEW_TYPE_1D && l1->vci.subresourceRange.baseArrayLayer == 1);
        CHECK(t->layer_view[0] == VK_NULL_HANDLE);
        vkn_tex_destroy_cookie(0x9A2);
    }
    // an explicit NVN format number wins over the engine format (CreateTexture's stack argument); BGRX8: alpha one in the view
    d = tdesc(4, 1, 8, 8, 1, 1, VKN_TC_SAMPLED, 0x9A3, ~0u); d.nvn_fmt = 0x9c;
    t = vkn_tex_create(H(97, 1), &d, NULL, NULL);
    CHECK(t && t->format == VK_FORMAT_B8G8R8A8_SRGB && t->nvn_fmt == 0x9c); vkn_tex_destroy_cookie(0x9A3);
    d.nvn_fmt = 0x9b; d.cookie = 0x9A4;
    t = vkn_tex_create(H(98, 1), &d, NULL, NULL);
    CHECK(t && t->format == VK_FORMAT_B8G8R8A8_SRGB); if (t) { mk_obj *v = view_of(t->view); CHECK(v && v->vci.components.a == VK_COMPONENT_SWIZZLE_ONE); }
    vkn_tex_destroy_cookie(0x9A4);
    d.nvn_fmt = 0x77; d.cookie = 0x9A5; const char *why = NULL;   // a number without a Vulkan equivalent
    CHECK(vkn_tex_create(H(99, 1), &d, NULL, &why) == NULL); CHECK(why && !strcmp(why, "format without a Vulkan equivalent"));
    // a handle index beyond the table: the texture exists (by cookie and pool id), unlinked
    d = tdesc(4, 1, 8, 8, 1, 1, VKN_TC_SAMPLED, 0x9A6, 1460);
    t = vkn_tex_create(H(1u << 17, 1), &d, NULL, NULL);
    CHECK(t != NULL); CHECK(vkn_tex_get(H(1u << 17, 1)) == NULL); CHECK(vkn_tex_by_cookie(0x9A6) == t); CHECK(t && vkn_tex_view_by_id(1460, NULL) == t->view);
    vkn_tex_destroy_cookie(0x9A6);
    flush_logged(); frames(3);
}

// ---- stage 4: the GPU BC route (vkn_bcgpu.c) on a device without BC: pack > GPU ASTC 4x4 > GPU RGBA8 > CPU ----
// The data doors (vkn_shlib_bc.cpp, moved there from xbg_game.cpp's [DOS2] VK-NATIVE export) are stubbed here: td_on = 0 (the default for every other test) is a build
// without them (blob NULL: no GPU route, the CPU decode as before). The self-tests themselves are device-side: here their verdicts are
// what the stubs say, the test checks what the backend does with them (one submission each, the zero re-read / re-run, route off).
static int td_on, td_dec_bad, td_enc_good = 1, td_enc_zero, td_dec_kat_calls, td_enc_kat_calls, td_enc_checks;
static const uint32_t td_blob[16] = {0x07230203};
const uint32_t *vkn_shlib_bc_blob(int which, size_t *bytes) {
    if (bytes) *bytes = td_on && which >= 0 && which <= 2 ? sizeof td_blob : 0;
    return td_on && which >= 0 && which <= 2 ? td_blob : NULL;
}
static int td_rgba8(uint32_t f) { return (f >= 0x41 && f <= 0x48) || f == 0x4d || f == 0x4e; }
int vkn_shlib_bc_kat_input(uint32_t fmt, void *in, size_t in_cap, size_t *in_bytes, size_t *out_bytes, uint32_t push[4]) {
    if (!td_rgba8(fmt) || in_cap < 64) return -1;
    td_dec_kat_calls++;
    memset(in, 0, 64); *in_bytes = 64; *out_bytes = 2048u * 64;
    push[0] = 1; push[1] = 2048; push[2] = 4; push[3] = fmt == 0x41 || fmt == 0x42 || fmt == 0x45 || fmt == 0x46 ? 2 : 4;
    return 0;
}
int vkn_shlib_bc_kat_check(uint32_t fmt, const void *out, size_t out_bytes, char *line, size_t cap) {
    (void)out; (void)out_bytes; if (line && cap) snprintf(line, cap, "stub");
    return fmt == (uint32_t)td_dec_bad ? 7 : 0;
}
int vkn_shlib_bca_kat_input(void *in, size_t in_cap, size_t *in_bytes, void *rgba, size_t rgba_cap, size_t *rgba_bytes, uint32_t push[][4], uint32_t max_disp,
                          uint32_t *ndisp, uint32_t *blocks) {
    if (in_cap < 256 || rgba_cap < 285 * 64 || max_disp < 6) return -1;
    td_enc_kat_calls++;
    memset(in, 0, 256); memset(rgba, 0, 285 * 64); *in_bytes = 256; *rgba_bytes = 285 * 64; *ndisp = 6; *blocks = 285;
    static const uint32_t first[7] = {0, 29, 77, 125, 173, 221, 285};
    for (int i = 0; i < 6; i++) { push[i][0] = 6; push[i][1] = first[i]; push[i][2] = first[i + 1]; push[i][3] = (i == 1 || i == 2 ? 1u : i == 3 || i == 4 ? 2u : 0u) | 4u << 8; }
    return 0;
}
int vkn_shlib_bca_kat_check(const void *enc, uint32_t cands, int *zero, char *line, size_t cap) {
    (void)enc; td_enc_checks++;
    if (zero) *zero = td_enc_zero > 0;
    if (td_enc_zero > 0) td_enc_zero--;
    if (line && cap) snprintf(line, cap, "stub candidates=%u", cands);
    return td_enc_good;
}
uint32_t vkn_shlib_bca_flags(uint32_t nvn) {   // xbg_bc_astc_flags
    switch (nvn) { case 0x41: case 0x42: case 0x45: case 0x46: return 1 | 2 << 8; case 0x43: case 0x44: case 0x47: case 0x48: return 2 | 4 << 8;
                   case 0x4d: case 0x4e: return 4 << 8; default: return 0; }
}
const char *vkn_shlib_bca_shape(uint32_t nvn, int target, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, int samples, int storage, int vt) {   // xbg_bc_astc_shape
    if (samples > 1) return "multisample";
    if (storage) return "storage";
    if (target == 0 || target == 2 || target == 3) return "not-2d";
    if (!vt && levels == 1 && w >= 4096 && h >= 4096) return "vt-cache-shape";
    if (!vt && (nvn == 0x44 || nvn == 0x48) && (target == 4 || layers > 1)) return "bc3-array";
    return NULL;
}
static void ev_clear(void) { pthread_mutex_lock(&mk_mu); mk_ev_base = mk_seq; pthread_mutex_unlock(&mk_mu); }
// The commands recorded since ev_clear, of the ops that matter here (barriers of other images left out), as a compact string.
static void ev_seq(VkImage img, char *out, size_t cap) {
    size_t at = 0; out[0] = 0;
    pthread_mutex_lock(&mk_mu);
    for (uint64_t q = mk_ev_base; q < mk_seq && at + 24 < cap; q++) {
        const mk_ev *e = &mk_evs[q - mk_ev_base]; const char *n = NULL;
        switch (e->op) {
        case MK_E_BARRIER: n = !e->a[0] ? "mb" : e->a[0] == MK_H(img) ? "ib" : NULL; break;
        case MK_E_COPY_BUF: n = "cb"; break;
        case MK_E_BIND_PIPE: n = "bp"; break;
        case MK_E_BIND_SETS: n = "bs"; break;
        case MK_E_PUSH: n = "pc"; break;
        case MK_E_DISPATCH: n = "d"; break;
        case MK_E_COPY_BUF_IMG: n = e->a[0] == MK_H(img) ? "ci" : NULL; break;
        default: break;
        }
        if (n) at += (size_t)snprintf(out + at, cap - at, "%s%s", at ? " " : "", n);
    }
    pthread_mutex_unlock(&mk_mu);
}
static const mk_ev *ev_nth(int op, int k) {   // the k-th entry of an op since ev_clear (NULL = none)
    const mk_ev *r = NULL;
    pthread_mutex_lock(&mk_mu);
    for (uint64_t q = mk_ev_base; q < mk_seq; q++) if (mk_evs[q - mk_ev_base].op == op && !k--) { r = &mk_evs[q - mk_ev_base]; break; }
    pthread_mutex_unlock(&mk_mu);
    return r;
}
static int ev_count(int op) { int n = 0; pthread_mutex_lock(&mk_mu); for (uint64_t q = mk_ev_base; q < mk_seq; q++) n += mk_evs[q - mk_ev_base].op == op; pthread_mutex_unlock(&mk_mu); return n; }
static vkn_tex *bc_lin(uint64_t handle, uint32_t nvn, uint32_t tg, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, const uint8_t *data, uint64_t cookie, uint32_t pool) {
    // linear BC data: every (level, layer) tight block rows of `data` (the same bytes for each)
    vkn_tex_region r[32]; uint32_t n = 0;
    const uint32_t ly = tg == 8 ? 6 : layers;
    for (uint32_t l = 0; l < ly; l++) for (uint32_t m = 0; m < levels && n < 32; m++) {
        r[n].level = m; r[n].layer = l; r[n].z = 0; r[n].w = (w >> m) ? (w >> m) : 1; r[n].h = (h >> m) ? (h >> m) : 1; r[n].d = 1; r[n].src = data; n++;
    }
    vkn_tex_desc d = tdesc(55, tg, w, h, levels, layers, VKN_TC_SAMPLED, cookie, pool); d.nvn_fmt = nvn;
    const vkn_tex_src s = {VKN_SRC_REGIONS, r, n, NULL, 0, 0, 0};
    const char *why = NULL; vkn_tex *t = vkn_tex_create(handle, &d, &s, &why);
    if (!t) printf("  bc_lin: %s\n", why ? why : "?");
    return t;
}
static void bcg_reset(const char *astc, const char *decode, const char *batch) {
    flush_logged();   // (vkn_bcgpu_reset: no job may be pending)
    if (astc) setenv("DM_GFX_BC_ASTC", astc, 1); else unsetenv("DM_GFX_BC_ASTC");
    if (decode) setenv("DM_GFX_BC_DECODE", decode, 1); else unsetenv("DM_GFX_BC_DECODE");
    if (batch) setenv("DM_GFX_BC_BATCH_MB", batch, 1); else unsetenv("DM_GFX_BC_BATCH_MB");
    vkn_bcgpu_reset();
}
static void test_tex_bcgpu(void) {
    char seq[1024]; vkn_bcgpu_stats g; vkn_tex_stats ts0, ts;
    static uint8_t data[1 << 20];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 31 + 7);
    mk_bc = 0; mk_astc = 7; td_on = 1; td_dec_bad = 0; td_enc_good = 1; td_enc_zero = 0;
    flush_logged(); frames(3);
    vkn_tex_get_stats(&ts0);

    // (a) auto on the mock's Qualcomm: route 2 off ("only simulated here"), route 3 = GPU decode to RGBA8; decoder self-test once per format
    bcg_reset(NULL, NULL, NULL);
    td_dec_kat_calls = td_enc_kat_calls = 0;
    const int sub0 = mk_submits;
    vkn_tex *A = bc_lin(H(400, 1), 0x44, 1, 16, 8, 2, 1, data, 0x4A0, 4000);   // BC3 16x8, 2 levels
    CHECK(A != NULL);
    vkn_bcgpu_get_stats(&g);
    CHECK_EQ(g.ready, 1); CHECK_EQ(g.astc, -1); CHECK_EQ(g.astc_off, VKN_BCAK_QCOM); CHECK_EQ(td_dec_kat_calls, 1); CHECK_EQ(td_enc_kat_calls, 0);
    CHECK_EQ(mk_submits - sub0, 1);   // one self-test submission (the format's pipeline)
    CHECK_EQ(g.kat_ok, 1u << (0x44 - 0x41)); CHECK_EQ(g.routed[VKN_BCG_RGBA8], 1); CHECK_EQ(g.kept[VKN_BCAK_QCOM], 1);
    if (A) {
        CHECK_EQ(A->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(A->bcgpu, VKN_BCG_RGBA8); CHECK_EQ(A->decoded, 1); CHECK_EQ(A->astc, 0);
        CHECK_EQ(vkn_upload_pending_bytes(), 2 * 32 + 128 + 32);   // one batch: table 2 regions x 32 bytes, then 8 + 2 blocks x 16
        mk_log_reset(); ev_clear(); CHECK(vkn_upload_flush() >= 1);
        ev_seq(A->image, seq, sizeof seq);
        CHECK(!strcmp(seq, "ib mb cb mb bp bs pc d mb ci ib"));
        if (strcmp(seq, "ib mb cb mb bp bs pc d mb ci ib")) printf("  seq: %s\n", seq);
        const mk_ev *pc = ev_nth(MK_E_PUSH, 0), *dp = ev_nth(MK_E_DISPATCH, 0), *cb = ev_nth(MK_E_COPY_BUF, 0);
        CHECK(pc && pc->u[0] == 2 && pc->u[1] == 10 && pc->u[2] == 4 && pc->u[3] == 4);   // regions, blocks (8 + 2), bytes a pixel, words a block
        CHECK(dp && dp->u[0] == 1);
        CHECK(cb && cb->off[1] == 0 && cb->off[2] == 2 * 32 + 128 + 32);
        // the staged input: the region table (input offsets past the table) and the blocks of both levels
        const uint8_t *st = cb ? mk_buf_cpu((VkBuffer)(uintptr_t)cb->a[0]) : NULL;
        CHECK(st != NULL);
        if (st) {
            const uint32_t *w = (const uint32_t *)(st + cb->off[0]);
            CHECK(w[0] == 0 && w[1] == 4 && w[2] == 8 && w[3] == 2 && w[4] == 16 && w[5] == 0 && w[6] == 16 && w[7] == 8);
            CHECK(w[8] == 8 && w[9] == 2 && w[10] == 2 && w[11] == 1 && w[12] == 16 + 32 && w[13] == 8 * 64 / 4 && w[14] == 8 && w[15] == 2);
            CHECK(!memcmp(st + cb->off[0] + 64, data, 128)); CHECK(!memcmp(st + cb->off[0] + 64 + 128, data, 32));
        }
        CHECK_EQ(copies_of(A->image), 2);
        const mk_copy_t *c0 = copy_of(A->image, 0, 0, 0), *c1 = copy_of(A->image, 1, 0, 0);
        CHECK(c0 && c1);
        if (c0 && c1) {
            CHECK_EQ(c0->c.bufferOffset, 0); CHECK_EQ(c0->c.bufferRowLength, 16); CHECK_EQ(c0->c.bufferImageHeight, 8); CHECK_EQ(c0->c.imageExtent.width, 16);
            CHECK_EQ(c0->c.imageExtent.height, 8); CHECK_EQ(c1->c.bufferOffset, 8 * 64); CHECK_EQ(c1->c.bufferRowLength, 8); CHECK_EQ(c1->c.bufferImageHeight, 4);
            CHECK_EQ(c1->c.imageExtent.width, 8); CHECK_EQ(c1->c.imageExtent.height, 4);
        }
        CHECK_EQ(A->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        // a CPU-side update of a GPU-RGBA8 texture: the CPU decoder into the same RGBA8 image (one copy, no dispatch)
        mk_log_reset(); ev_clear();
        CHECK_EQ(vkn_tex_update(A, 0, 0, 4, 4, 4, 4, data, 16), 0); vkn_upload_flush();
        CHECK_EQ(ev_count(MK_E_DISPATCH), 0); CHECK_EQ(copies_of(A->image), 1);
    }
    vkn_bcgpu_get_stats(&g); CHECK_EQ(g.jobs, 1); CHECK_EQ(g.batches, 1); CHECK_EQ(g.recorded, 1); CHECK_EQ(g.dec_dispatch, 1); CHECK_EQ(g.enc_dispatch, 0); CHECK_EQ(g.blocks, 10);
    // the same format again: no second self-test; BC4 / BC6H -> CPU; a format whose decode fails its self-test -> CPU
    td_dec_bad = 0x41;
    vkn_tex *B = bc_lin(H(401, 1), 0x44, 1, 8, 8, 1, 1, data, 0x4A1, 4001), *C = bc_lin(H(402, 1), 0x49, 1, 8, 8, 1, 1, data, 0x4A2, 4002),
            *D = bc_lin(H(403, 1), 0x41, 1, 8, 8, 1, 1, data, 0x4A3, 4003), *E = bc_lin(H(404, 1), 0x50, 1, 8, 8, 1, 1, data, 0x4A4, 4004);
    CHECK(B && C && D && E);
    vkn_bcgpu_get_stats(&g);
    CHECK_EQ(td_dec_kat_calls, 2); CHECK_EQ(g.kat_bad, 1u << 0); CHECK_EQ(g.why[VKN_BCGW_FMT], 2); CHECK_EQ(g.why[VKN_BCGW_PIPE], 1);
    if (B) { CHECK_EQ(B->bcgpu, VKN_BCG_RGBA8); }
    if (C) { CHECK_EQ(C->bcgpu, 0); CHECK_EQ(C->decoded, 1); CHECK_EQ(C->format, VK_FORMAT_R8_UNORM); }
    if (D) { CHECK_EQ(D->bcgpu, 0); CHECK_EQ(D->decoded, 1); CHECK_EQ(D->format, VK_FORMAT_R8G8B8A8_UNORM); }
    if (E) { CHECK_EQ(E->bcgpu, 0); CHECK_EQ(E->format, VK_FORMAT_R16G16B16A16_SFLOAT); }
    ev_clear(); flush_logged();
    if (D) CHECK_EQ(copies_of(D->image), 1);
    CHECK_EQ(ev_count(MK_E_DISPATCH), 1);   // B's decode; C / D / E decoded on the CPU at creation
    td_dec_bad = 0;
    vkn_tex_destroy_cookie(0x4A0); vkn_tex_destroy_cookie(0x4A1); vkn_tex_destroy_cookie(0x4A2); vkn_tex_destroy_cookie(0x4A3); vkn_tex_destroy_cookie(0x4A4);

    // (b) DM_GFX_BC_ASTC=1: route 2 - decode, encode, ASTC blocks into an ASTC 4x4 image; the encoder self-test once (zero re-read + re-run)
    bcg_reset("1", NULL, NULL);
    td_dec_kat_calls = td_enc_kat_calls = td_enc_checks = 0; td_enc_zero = 2;
    const int sub1 = mk_submits;
    vkn_tex *F = bc_lin(H(405, 1), 0x45, 1, 16, 8, 1, 1, data, 0x4A5, 4005);   // BC1 sRGB
    CHECK(F != NULL);
    vkn_bcgpu_get_stats(&g);
    CHECK_EQ(g.astc, 1); CHECK_EQ(td_enc_kat_calls, 1); CHECK_EQ(td_enc_checks, 3);   // read, re-read (zero), re-run
    CHECK_EQ(mk_submits - sub1, 1 + 2 + 1);   // the format's decoder test, the encoder test twice, the twin's decoder test (family)
    CHECK_EQ(td_dec_kat_calls, 2);            // 0x45 and its linear twin 0x41 (both pipelines of the family)
    if (F) {
        CHECK_EQ(F->format, VK_FORMAT_ASTC_4x4_SRGB_BLOCK); CHECK_EQ(F->astc, 4); CHECK_EQ(F->bcgpu, VKN_BCG_ASTC); CHECK_EQ(F->decoded, 0);
        mk_log_reset(); ev_clear(); vkn_upload_flush();
        ev_seq(F->image, seq, sizeof seq);
        CHECK(!strcmp(seq, "ib mb cb mb bp bs pc d mb bp bs pc d mb ci ib"));
        if (strcmp(seq, "ib mb cb mb bp bs pc d mb bp bs pc d mb ci ib")) printf("  seq: %s\n", seq);
        const mk_ev *pd = ev_nth(MK_E_PUSH, 0), *pe = ev_nth(MK_E_PUSH, 1), *b0 = ev_nth(MK_E_BIND_PIPE, 0), *b1 = ev_nth(MK_E_BIND_PIPE, 1);
        CHECK(pd && pd->u[0] == 1 && pd->u[1] == 8 && pd->u[2] == 4 && pd->u[3] == 2);
        CHECK(pe && pe->u[0] == 1 && pe->u[1] == 0 && pe->u[2] == 8 && pe->u[3] == (1 | 2 << 8));   // regions, first, end, flags (direct BC1, 2 words)
        CHECK(b0 && b1 && b0->a[0] != b1->a[0] && b0->u[0] == VK_PIPELINE_BIND_POINT_COMPUTE);
        const mk_copy_t *c0 = copy_of(F->image, 0, 0, 0);
        CHECK(c0 && c0->c.bufferOffset == 0 && c0->c.bufferRowLength == 16 && c0->c.bufferImageHeight == 8 && c0->c.imageExtent.width == 16);
        CHECK(c0 && c0->buf != (VkBuffer)(uintptr_t)ev_nth(MK_E_COPY_BUF, 0)->a[1]);   // from the ASTC scratch, not the input scratch
        // an update of a GPU-ASTC texture: its BC rows through the GPU decode + encode (one region at 4, 4)
        mk_log_reset(); ev_clear();
        vkn_tex_update_stats us0; vkn_tex_update_stats_get(&us0);
        CHECK_EQ(vkn_tex_update(F, 0, 0, 4, 4, 8, 4, data, 16), 0); vkn_upload_flush();
        CHECK_EQ(ev_count(MK_E_DISPATCH), 2);
        const mk_copy_t *u0 = copy_of(F->image, 0, 0, 0);
        CHECK(u0 && u0->c.imageOffset.x == 4 && u0->c.imageOffset.y == 4 && u0->c.imageExtent.width == 8 && u0->c.imageExtent.height == 4);
        vkn_tex_update_stats us; vkn_tex_update_stats_get(&us); CHECK_EQ(us.dropped_fmt, us0.dropped_fmt); CHECK_EQ(us.queued, us0.queued + 1);
    }
    // BC7 / BC2 ASTC; a BC3 array stays RGBA8 (the module's shape rule), a storage texture too; image properties refused -> RGBA8
    vkn_tex *G = bc_lin(H(406, 1), 0x4d, 1, 8, 8, 1, 1, data, 0x4A6, 4006), *I = bc_lin(H(407, 1), 0x44, 4, 8, 8, 1, 2, data, 0x4A7, 4007);
    mk_img_props_fail = 1;
    vkn_tex *J = bc_lin(H(408, 1), 0x43, 1, 8, 8, 1, 1, data, 0x4A8, 4008);
    mk_img_props_fail = 0;
    CHECK(G && I && J);
    if (G) { CHECK_EQ(G->bcgpu, VKN_BCG_ASTC); CHECK_EQ(G->format, VK_FORMAT_ASTC_4x4_UNORM_BLOCK); }
    if (I) { CHECK_EQ(I->bcgpu, VKN_BCG_RGBA8); CHECK_EQ(I->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(I->layers, 2); }
    if (J) { CHECK_EQ(J->bcgpu, VKN_BCG_RGBA8); }
    vkn_bcgpu_get_stats(&g); CHECK_EQ(g.kept[VKN_BCAK_SHAPE], 1); CHECK_EQ(g.kept[VKN_BCAK_PROPS], 1);
    flush_logged();
    if (I) { CHECK_EQ(copies_of(I->image), 2); const mk_copy_t *l1 = copy_of(I->image, 0, 1, 0); CHECK(l1 && l1->c.imageSubresource.baseArrayLayer == 1); }
    vkn_tex_get_stats(&ts);
    CHECK_EQ(ts.bc_made[VKN_BC_GPU_ASTC] - ts0.bc_made[VKN_BC_GPU_ASTC], 2); CHECK_EQ(ts.bc_made[VKN_BC_GPU_RGBA8] - ts0.bc_made[VKN_BC_GPU_RGBA8], 4);
    CHECK_EQ(ts.bc_alive[VKN_BC_GPU_ASTC], 2); CHECK(ts.bc_bytes[VKN_BC_GPU_ASTC] > 0);
    for (uint64_t c = 0x4A5; c <= 0x4A8; c++) vkn_tex_destroy_cookie(c);

    // (c) the encoder self-test fails: route 2 off for the process, the same textures RGBA8
    bcg_reset("1", NULL, NULL); td_enc_good = 0; td_enc_zero = 0;
    vkn_tex *K = bc_lin(H(409, 1), 0x44, 1, 8, 8, 1, 1, data, 0x4A9, 4009);
    CHECK(K && K->bcgpu == VKN_BCG_RGBA8);
    vkn_bcgpu_get_stats(&g); CHECK_EQ(g.astc, -1); CHECK_EQ(g.astc_off, VKN_BCAK_ENC); CHECK_EQ(g.kept[VKN_BCAK_ENC], 1);
    td_enc_good = 1;
    vkn_tex_destroy_cookie(0x4A9);
    // DM_GFX_BC_ASTC=0 -> RGBA8; DM_GFX_BC_DECODE=cpu -> CPU; no doors -> CPU
    bcg_reset("0", NULL, NULL);
    vkn_tex *L = bc_lin(H(410, 1), 0x44, 1, 8, 8, 1, 1, data, 0x4AA, 4010);
    CHECK(L && L->bcgpu == VKN_BCG_RGBA8); vkn_bcgpu_get_stats(&g); CHECK_EQ(g.astc_off, VKN_BCAK_OFF);
    bcg_reset("1", "cpu", NULL);
    vkn_tex *M = bc_lin(H(411, 1), 0x44, 1, 8, 8, 1, 1, data, 0x4AB, 4011);
    CHECK(M && M->bcgpu == 0 && M->decoded == 1); vkn_bcgpu_get_stats(&g); CHECK_EQ(g.ready, -1); CHECK_EQ(g.why[VKN_BCGW_OFF], 1);
    td_on = 0; bcg_reset("1", NULL, NULL);
    vkn_tex *N = bc_lin(H(412, 1), 0x44, 1, 8, 8, 1, 1, data, 0x4AC, 4012);
    CHECK(N && N->bcgpu == 0 && N->decoded == 1); vkn_bcgpu_get_stats(&g); CHECK_EQ(g.why[VKN_BCGW_DOOR], 1);
    td_on = 1;
    flush_logged();
    for (uint64_t c = 0x4AA; c <= 0x4AC; c++) vkn_tex_destroy_cookie(c);

    // (d) a level bigger than a batch: bands of block rows, one batch each (DM_GFX_BC_BATCH_MB=4: 4 MB of RGBA8 a batch)
    bcg_reset("1", NULL, "4");
    vkn_tex *P = bc_lin(H(413, 1), 0x41, 1, 2048, 1024, 1, 1, data, 0x4AD, 4013);   // BC1: 512 x 256 blocks, 8 MB decoded
    CHECK(P != NULL);
    if (P) {
        mk_log_reset(); ev_clear(); vkn_upload_flush();
        CHECK_EQ(ev_count(MK_E_DISPATCH), 4);   // 2 bands x (decode + encode)
        CHECK_EQ(copies_of(P->image), 2);
        const mk_copy_t *a = &mk_copy[0], *b = &mk_copy[1];
        CHECK(a->c.imageOffset.y == 0 && a->c.imageExtent.height == 512 && b->c.imageOffset.y == 512 && b->c.imageExtent.height == 512);
        CHECK(a->c.bufferOffset == 0 && b->c.bufferOffset == 0 && a->c.bufferImageHeight == 512 && a->c.bufferRowLength == 2048);
        vkn_tex_destroy_cookie(0x4AD);
    }

    // (e) the pack first: a texture the pack holds stays from the pack with the GPU route on; its mip reduction (made without data:
    // GPU ASTC 4x4 by its own route) is re-made in the pack entry's 6x6 and gets the tail copy
    bcg_reset("1", NULL, NULL);
    {
        const char *path = "/tmp/vkn_bcgpu_test.pack";
        uint64_t ns_ = 0; uint8_t *sx = tp_storage(0x42, 24, 24, 2, 1, 4, &ns_);
        uint8_t dy[320]; memset(dy, 0x41, sizeof dy);   // 6x6 blocks: 4 x 4 x 16 + 2 x 2 x 16
        tp_ent e1 = tp_of(sx, ns_, 0x42, 24, 24, 2, 6, dy, 256 + 64);
        tp_write(path, &e1, 1, "D3AC");
        setenv("DM_ASTC_CACHE_PATH", path, 1); unsetenv("DM_VK_NATIVE_ASTC"); vkn_astc_reset();
        const char *why = NULL;
        vkn_tex *Q = tp_create(H(414, 1), 0x42, 24, 24, 2, 1, sx, ns_, 0x4AE, 4014, &why);
        CHECK(Q && Q->astc == 6 && Q->bcgpu == 0 && Q->format == VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
        vkn_tex_desc dn = tdesc(55, 1, 12, 12, 1, 1, VKN_TC_SAMPLED, 0x4AF, 4015); dn.nvn_fmt = 0x42;
        vkn_tex *R = vkn_tex_create(H(415, 1), &dn, NULL, &why);
        CHECK(R && R->bcgpu == VKN_BCG_ASTC && R->astc == 4);
        flush_logged();
        vkn_tex_stats t1; vkn_tex_get_stats(&t1);
        CHECK_EQ(vkn_tex_swap_reduced(H(414, 1), 0x4AE, 0x4AF), 0);
        vkn_tex *R2 = vkn_tex_get(H(414, 1));
        CHECK(R2 && R2 != R && R2->astc == 6 && R2->bcgpu == 0 && R2->format == VK_FORMAT_ASTC_6x6_UNORM_BLOCK);
        vkn_tex_get_stats(&ts); CHECK_EQ(ts.pack_tail - t1.pack_tail, 1);
        flush_logged();
        CHECK_EQ(mk_nicopy, 1);
        vkn_tex_destroy_cookie(0x4AE); vkn_tex_destroy_cookie(0x4AF);
        unsetenv("DM_ASTC_CACHE_PATH"); vkn_astc_reset(); remove(path); free(sx);
    }

    // back to a build without doors for everything after this; the module's device objects go
    flush_logged(); frames(3);
    td_on = 0; bcg_reset(NULL, NULL, NULL);
    unsetenv("DM_GFX_BC_ASTC"); unsetenv("DM_GFX_BC_DECODE"); unsetenv("DM_GFX_BC_BATCH_MB");
    mk_bc = 1;
}
static void test_textures(void) {
    test_tex_shapes();
    test_tex_formats();
    test_tex_linear();
    test_tex_widen_bc();
    test_tex_packaged();
    test_tex_astc_pack();
    test_tex_bcgpu();
    test_tex_targets();
    test_tex_depth_standin();   // [VK-DRV]
    test_tex_lifetime();
    test_tex_shared();
    test_samplers();
    flush_logged(); frames(3);
    CHECK_EQ(mk_live(MK_IMAGE), 0); CHECK_EQ(mk_live(MK_VIEW), 0); CHECK_EQ(mk_live(MK_SAMPLER), 0);
    vkn_tex_stats ts; vkn_tex_get_stats(&ts);
    CHECK_EQ(ts.alive[0] + ts.alive[1] + ts.alive[2] + ts.alive[3], 0); CHECK_EQ(ts.pending, 0);
    CHECK_EQ(used_of(VKN_ARENA_STAGING), 0);
}
// ================================================================================================================ state
// Expected tables, from MAP-1 §3.2 (engine value -> NVN number) and the NVN enums (NVN number -> Vulkan); out-of-range values as
// 0x922530 / 0x922d10 / 0x923360 treat them ((v - 1) as a byte beyond the table -> NVN 1).
static VkBlendFactor exp_blend(uint32_t e) {
    static const uint8_t nvn[11] = {1, 2, 3, 4, 9, 10, 5, 6, 7, 8, 11};   // MAP-1 "Множитель смешивания"
    static const VkBlendFactor vk[12] = {VK_BLEND_FACTOR_ZERO, /*1*/ VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        /*5*/ VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        /*9*/ VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};   // NVNblendFunc 1..11
    return vk[e <= 10 ? nvn[e] : 1];
}
static VkBlendOp exp_alpha_op(uint32_t op, int sep) {   // 1..4 -> NVN 2..5 (sub, reverse sub, min, max), else 1 (add); without bit 0: add
    static const VkBlendOp vk[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return vk[sep && op >= 1 && op <= 4 ? op + 1 : 1];
}
static VkCompareOp exp_cmp(uint32_t e) {   // engine n -> NVN n + 1 (1 never .. 8 always); beyond 7 -> NVN 1
    static const VkCompareOp vk[9] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                      VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return vk[e <= 7 ? e + 1 : 1];
}
static VkStencilOp exp_sop(uint32_t e) {   // MAP-1 "Действие трафарета": 0 keep(1) 1 zero(2) 2 replace(3) 3 invert(6) 4 incr wrap(7) 5 decr wrap(8) 6 incr(4) 7 decr(5)
    static const uint8_t nvn[8] = {1, 2, 3, 6, 7, 8, 4, 5};
    static const VkStencilOp vk[9] = {VK_STENCIL_OP_KEEP, /*1*/ VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                      VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};   // NVNstencilOp 1..8
    return vk[e <= 7 ? nvn[e] : 1];
}
// MAP-1 §3.1: component type -> NVN formats for 1..4 components, bytes per component, scalar class (1 float, 2 sint, 3 uint)
static const uint8_t k_doc_vtx_nvn[12][4] = {{0x0a, 0x16, 0x22, 0x2e}, {0x0b, 0x17, 0x23, 0x2f}, {0x0c, 0x18, 0x24, 0x30}, {0x05, 0x11, 0x1d, 0x29},
    {0x06, 0x12, 0x1e, 0x2a}, {0x08, 0x14, 0x20, 0x2c}, {0x07, 0x13, 0x1f, 0x2b}, {0x09, 0x15, 0x21, 0x2d}, {0x01, 0x0d, 0x19, 0x25}, {0x03, 0x0f, 0x1b, 0x27},
    {0x02, 0x0e, 0x1a, 0x26}, {0x04, 0x10, 0x1c, 0x28}};
static const uint8_t k_doc_vtx_size[12] = {4, 4, 4, 2, 2, 2, 2, 2, 1, 1, 1, 1};
static const uint8_t k_doc_vtx_cls[12] = {1, 3, 2, 1, 1, 3, 1, 2, 1, 3, 1, 2};
static int exp_loc(uint32_t usage, uint32_t index) {
    switch (usage) { case 2: return 4 + (int)index; case 3: return 1; case 4: return 3; case 5: return 2; case 6: return 14; case 7: return 15; case 8: return 9 + (int)index; default: return 0; }
}
static const VkVertexInputAttributeDescription *attr_at(const vkn_vfmt *f, uint32_t loc) { for (uint32_t i = 0; i < f->nattr; i++) if (f->attr[i].location == loc) return &f->attr[i]; return NULL; }
static const VkVertexInputBindingDescription *bind_at(const vkn_vfmt *f, uint32_t b) { for (uint32_t i = 0; i < f->nbind; i++) if (f->bind[i].binding == b) return &f->bind[i]; return NULL; }
// The 0x9282c0 lists (MAP-2 §ChannelMaskForFormat)
static VkColorComponentFlags exp_mask(uint32_t n) {
    const VkColorComponentFlags R = VK_COLOR_COMPONENT_R_BIT, RG = R | VK_COLOR_COMPONENT_G_BIT, RGB = RG | VK_COLOR_COMPONENT_B_BIT, RGBA = RGB | VK_COLOR_COMPONENT_A_BIT;
    if (n == 1 || (n >= 3 && n <= 6) || (n >= 8 && n <= 0xc) || (n >= 0x32 && n <= 0x34)) return R;
    if (n == 0xd || (n >= 0xf && n <= 0x12) || (n >= 0x14 && n <= 0x18) || n == 0x35) return RG;
    if (n == 0x19 || n == 0x1d || n == 0x1e || (n >= 0x20 && n <= 0x24) || n == 0x3f || n == 0x95 || n == 0x99) return RGB;
    if (n == 0x25 || n == 0x26 || (n >= 0x29 && n <= 0x30) || n == 0x38 || n == 0x39 || n == 0x3b || n == 0x3d || n == 0x98 || n == 0x9a || n == 0x9c) return RGBA;
    return 0;   // SetRenderTargets reset the mask to 0000; nothing sets it for other formats
}

static void test_state_objects(void) {
    // blend: colour factors (non-separate: alpha = colour, add / add)
    for (uint32_t f = 0; f < 12; f++) {
        const uint32_t g = (f * 5 + 3) % 12;
        const uint16_t id = vkn_blend_put(H(100 + f, 1), (uint8_t)f, (uint8_t)g, 9, 9, 3, 0);
        CHECK(id != 0); const VkPipelineColorBlendAttachmentState *a = vkn_blend_att(id); CHECK(a != NULL); if (!a) continue;
        CHECK_EQ(a->srcColorBlendFactor, exp_blend(f)); CHECK_EQ(a->dstColorBlendFactor, exp_blend(g));
        CHECK_EQ(a->srcAlphaBlendFactor, exp_blend(f)); CHECK_EQ(a->dstAlphaBlendFactor, exp_blend(g));
        CHECK_EQ(a->colorBlendOp, VK_BLEND_OP_ADD); CHECK_EQ(a->alphaBlendOp, VK_BLEND_OP_ADD);
        CHECK_EQ(a->colorWriteMask, 0xf);
        // blending is always on in the engine; one / zero / add is the same result as off (the code turns it off)
        CHECK_EQ(a->blendEnable, !(exp_blend(f) == VK_BLEND_FACTOR_ONE && exp_blend(g) == VK_BLEND_FACTOR_ZERO));
        CHECK_EQ(vkn_blend_id(H(100 + f, 1)), id);
    }
    CHECK_EQ(vkn_blend_att(vkn_blend_put(H(120, 1), 1, 0, 0, 0, 0, 0))->blendEnable, VK_FALSE);
    // separate alpha: factors and every alpha op
    for (uint32_t op = 0; op < 6; op++) for (uint32_t f = 0; f < 12; f++) {
        const uint16_t id = vkn_blend_put(H(200 + op * 12 + f, 1), 6, 7, (uint8_t)f, (uint8_t)((f + 2) % 12), (uint8_t)op, 1 | 0x10);
        const VkPipelineColorBlendAttachmentState *a = vkn_blend_att(id); CHECK(a != NULL); if (!a) continue;
        CHECK_EQ(a->srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA); CHECK_EQ(a->dstColorBlendFactor, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        CHECK_EQ(a->srcAlphaBlendFactor, exp_blend(f)); CHECK_EQ(a->dstAlphaBlendFactor, exp_blend((f + 2) % 12));
        CHECK_EQ(a->colorBlendOp, VK_BLEND_OP_ADD); CHECK_EQ(a->alphaBlendOp, exp_alpha_op(op, 1)); CHECK_EQ(a->blendEnable, VK_TRUE);
    }
    // dedup: equal arguments share an id, alpha arguments without the separate bit do not count; kill drops only the mapping
    const uint16_t b1 = vkn_blend_put(H(300, 1), 6, 7, 9, 9, 3, 0), b2 = vkn_blend_put(H(301, 1), 6, 7, 1, 2, 4, 0), b3 = vkn_blend_put(H(302, 1), 6, 7, 1, 2, 4, 1);
    CHECK_EQ(b1, b2); CHECK(b3 != b1);
    CHECK_EQ(vkn_blend_kill(H(300, 1)), 1); CHECK_EQ(vkn_blend_kill(H(300, 1)), 0); CHECK_EQ(vkn_blend_id(H(300, 1)), 0); CHECK_EQ(vkn_blend_id(H(301, 1)), b1);
    CHECK(vkn_blend_att(b1) != NULL); CHECK(vkn_blend_att(0) == NULL); CHECK(vkn_blend_att(250) == NULL);
    const uint16_t b4 = vkn_blend_put(H(301, 2), 2, 3, 0, 0, 0, 0);   // a new generation of the index
    CHECK_EQ(vkn_blend_id(H(301, 1)), 0); CHECK_EQ(vkn_blend_id(H(301, 2)), b4);
    vkn_state_stats ss; vkn_state_stats_get(&ss); const uint64_t bad0 = ss.bad;
    CHECK(vkn_blend_put(H(5000, 1), 6, 7, 0, 0, 0, 0) == b1); CHECK_EQ(vkn_blend_id(H(5000, 1)), 0);   // index beyond the table: content, no mapping
    vkn_state_stats_get(&ss); CHECK_EQ(ss.bad, bad0 + 1);
    // depth-stencil: every function and stencil op (both faces alike, fail op keep, the byte = compare and write mask)
    for (uint32_t f = 0; f < 9; f++) {
        const uint32_t sf = (f + 3) % 9, dfail = f, pass = (f + 1) % 9;
        const uint16_t id = vkn_ds_put(H(400 + f, 1), 1, 3, (uint8_t)f, 1, 0x5a, (uint8_t)sf, (uint8_t)dfail, (uint8_t)pass);
        const VkPipelineDepthStencilStateCreateInfo *d = vkn_ds_info(id); CHECK(d != NULL); if (!d) continue;
        CHECK_EQ(d->sType, VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        CHECK_EQ(d->depthTestEnable, 1); CHECK_EQ(d->depthWriteEnable, 1); CHECK_EQ(d->depthCompareOp, exp_cmp(f)); CHECK_EQ(d->stencilTestEnable, 1);
        CHECK_EQ(d->depthBoundsTestEnable, 0); CHECK(d->minDepthBounds == 0.0f && d->maxDepthBounds == 1.0f);
        CHECK_EQ(d->front.failOp, VK_STENCIL_OP_KEEP); CHECK_EQ(d->front.depthFailOp, exp_sop(dfail)); CHECK_EQ(d->front.passOp, exp_sop(pass));
        CHECK_EQ(d->front.compareOp, exp_cmp(sf)); CHECK_EQ(d->front.compareMask, 0x5a); CHECK_EQ(d->front.writeMask, 0x5a); CHECK_EQ(d->front.reference, 0);
        CHECK(!memcmp(&d->front, &d->back, sizeof d->front));
    }
    {   // the flags are bit 0 only (0x922d10: & 1)
        const VkPipelineDepthStencilStateCreateInfo *d = vkn_ds_info(vkn_ds_put(H(420, 1), 2, 0, 1, 2, 0xff, 7, 0, 0));
        CHECK(d && !d->depthTestEnable && !d->depthWriteEnable && !d->stencilTestEnable);
        CHECK_EQ(vkn_ds_put(H(421, 1), 1, 3, 2, 1, 0x5a, 5, 2, 3), vkn_ds_put(H(422, 1), 3, 1, 2, 3, 0x5a, 5, 2, 3));   // same content
        CHECK_EQ(vkn_ds_kill(H(421, 1)), 1); CHECK_EQ(vkn_ds_id(H(421, 1)), 0); CHECK(vkn_ds_id(H(422, 1)) != 0);
    }
    // raster: cull, fill, front face, depth bias (0x93d9f0: factor -2.5, units -5, clamp 0)
    for (uint32_t cull = 0; cull < 4; cull++) for (uint32_t fill = 0; fill < 4; fill++) for (uint32_t po = 0; po < 2; po++) {
        const uint16_t id = vkn_raster_put(H(500 + cull * 8 + fill * 2 + po, 1), (uint8_t)cull, (uint8_t)fill, 0, 0, po | 0x10);
        const VkPipelineRasterizationStateCreateInfo *r = vkn_raster_info(id); CHECK(r != NULL); if (!r) continue;
        CHECK_EQ(r->cullMode, cull == 1 ? VK_CULL_MODE_BACK_BIT : cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
        CHECK_EQ(r->polygonMode, fill == 0 ? VK_POLYGON_MODE_FILL : fill == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_POINT);
        CHECK_EQ(r->frontFace, VK_FRONT_FACE_CLOCKWISE); CHECK_EQ(r->depthBiasEnable, po); CHECK(r->lineWidth == 1.0f);
        CHECK_EQ(r->depthClampEnable, 0); CHECK_EQ(r->rasterizerDiscardEnable, 0);
        if (po) CHECK(r->depthBiasSlopeFactor == -2.5f && r->depthBiasConstantFactor == -5.0f && r->depthBiasClamp == 0.0f);
        else CHECK(r->depthBiasSlopeFactor == 0.0f && r->depthBiasConstantFactor == 0.0f);
    }
    {   // flags A / B: part of the identity, no Vulkan effect; the bias setter reaches every state with the bit
        const uint16_t ra = vkn_raster_put(H(540, 1), 1, 0, 1, 0, 1), rb = vkn_raster_put(H(541, 1), 1, 0, 0, 1, 1), rc = vkn_raster_put(H(542, 1), 1, 0, 0, 0, 1);
        CHECK(ra != rb && rb != rc && ra != rc);
        CHECK_EQ(vkn_raster_info(ra)->cullMode, vkn_raster_info(rc)->cullMode); CHECK_EQ(vkn_raster_info(rb)->depthBiasEnable, 1);
        vkn_state_set_depth_bias(-1.0f, -2.0f, 0.5f, 2.0f);
        CHECK(vkn_raster_info(rc)->depthBiasSlopeFactor == -1.0f && vkn_raster_info(rc)->depthBiasConstantFactor == -4.0f && vkn_raster_info(rc)->depthBiasClamp == 0.5f);
        CHECK(vkn_raster_info(vkn_raster_id(H(500, 1)))->depthBiasConstantFactor == 0.0f);   // no bias bit: untouched
        vkn_state_set_depth_bias(-2.5f, -5.0f, 0.0f, 1.0f);
        CHECK(vkn_raster_info(rc)->depthBiasSlopeFactor == -2.5f && vkn_raster_info(rc)->depthBiasConstantFactor == -5.0f);
        CHECK_EQ(vkn_raster_kill(H(540, 1)), 1); CHECK_EQ(vkn_raster_id(H(540, 1)), 0);
    }
}

static void test_vfmt(void) {
    mk_vtx3 = 1;
    // every component type x count, one element at location 0
    for (uint32_t t = 0; t < 12; t++) for (uint32_t n = 1; n <= 4; n++) {
        const uint8_t el[6] = {0, 0, 0, 0, (uint8_t)t, (uint8_t)n};
        const uint16_t id = vkn_vfmt_put(H(600 + t * 4 + n, 1), el, 1);
        const vkn_vfmt *f = vkn_vfmt_info(id); CHECK(f != NULL); if (!f) continue;
        int x; CHECK_EQ(f->nattr, 1); CHECK_EQ(f->attr[0].location, 0); CHECK_EQ(f->attr[0].format, exp_nvn_vk(k_doc_vtx_nvn[t][n - 1], &x));
        CHECK_EQ(f->attr[0].offset, 0); CHECK_EQ(f->attr[0].binding, 0);
        CHECK_EQ(f->nbind, 1); CHECK_EQ(f->bind[0].stride, k_doc_vtx_size[t] * n); CHECK_EQ(f->bind[0].inputRate, VK_VERTEX_INPUT_RATE_VERTEX);
        CHECK_EQ(f->loc_class[0], k_doc_vtx_cls[t]); CHECK_EQ(f->loc_comps[0], n); CHECK_EQ(f->loc_mask, 1); CHECK_EQ(f->widened, 0);
    }
    // usage -> location
    for (uint32_t u = 0; u < 10; u++) for (uint32_t i = 0; i < 3; i++) {
        const uint8_t el[6] = {0, (uint8_t)u, (uint8_t)i, 0, 0, 1};
        const vkn_vfmt *f = vkn_vfmt_info(vkn_vfmt_put(H(700 + u * 3 + i, 1), el, 1)); CHECK(f != NULL); if (!f) continue;
        CHECK_EQ(f->nattr, 1); CHECK_EQ(f->attr[0].location, exp_loc(u, i)); CHECK_EQ(f->loc_mask, 1u << exp_loc(u, i));
    }
    // the engine's fullscreen mesh format (0x92bf50: 3 elements at main.raw 0x1f58698 = 00 01 00 00 00 02 | 00 02 00 00 00 02 | 00 03 00 00 00 01)
    {
        const uint8_t el[18] = {0, 1, 0, 0, 0, 2, 0, 2, 0, 0, 0, 2, 0, 3, 0, 0, 0, 1};
        const vkn_vfmt *f = vkn_vfmt_info(vkn_vfmt_put(H(800, 1), el, 3)); CHECK(f != NULL);
        if (f) {
            CHECK_EQ(f->nattr, 3); CHECK_EQ(f->attr[0].location, 0); CHECK_EQ(f->attr[1].location, 1); CHECK_EQ(f->attr[2].location, 4);   // sorted by location
            CHECK_EQ(attr_at(f, 0)->format, VK_FORMAT_R32G32_SFLOAT); CHECK_EQ(attr_at(f, 0)->offset, 0);
            CHECK_EQ(attr_at(f, 4)->format, VK_FORMAT_R32G32_SFLOAT); CHECK_EQ(attr_at(f, 4)->offset, 8);
            CHECK_EQ(attr_at(f, 1)->format, VK_FORMAT_R32_SFLOAT); CHECK_EQ(attr_at(f, 1)->offset, 16);
            CHECK_EQ(f->nbind, 1); CHECK_EQ(f->bind[0].stride, 20); CHECK_EQ(f->stride[0], 20); CHECK_EQ(f->stride[1], 0);
        }
    }
    // a mesh with two streams
    const uint8_t mesh[30] = {0, 0, 0, 0, 0, 3, 0, 3, 0, 0, 8, 4, 0, 2, 0, 0, 3, 2, 0, 2, 1, 0, 3, 2, 1, 4, 0, 0, 10, 4};
    const uint16_t mesh_id = vkn_vfmt_put(H(801, 1), mesh, 5);
    {
        const vkn_vfmt *f = vkn_vfmt_info(mesh_id); CHECK(f != NULL);
        if (f) {
            CHECK_EQ(f->nattr, 5); CHECK_EQ(f->loc_mask, (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5));
            CHECK_EQ(attr_at(f, 0)->format, VK_FORMAT_R32G32B32_SFLOAT); CHECK_EQ(attr_at(f, 0)->offset, 0);
            CHECK_EQ(attr_at(f, 1)->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(attr_at(f, 1)->offset, 12);
            CHECK_EQ(attr_at(f, 4)->format, VK_FORMAT_R16G16_SFLOAT); CHECK_EQ(attr_at(f, 4)->offset, 16);
            CHECK_EQ(attr_at(f, 5)->format, VK_FORMAT_R16G16_SFLOAT); CHECK_EQ(attr_at(f, 5)->offset, 20);
            CHECK_EQ(attr_at(f, 3)->format, VK_FORMAT_R8G8B8A8_SNORM); CHECK_EQ(attr_at(f, 3)->offset, 0); CHECK_EQ(attr_at(f, 3)->binding, 1);
            CHECK_EQ(f->nbind, 2); CHECK_EQ(bind_at(f, 0)->stride, 24); CHECK_EQ(bind_at(f, 1)->stride, 4);
            CHECK_EQ(bind_at(f, 1)->inputRate, VK_VERTEX_INPUT_RATE_VERTEX); CHECK_EQ(f->loc_class[1], 1); CHECK_EQ(f->loc_class[3], 1);
        }
    }
    // instancing: a stream is per instance when one of its elements is
    {
        const uint8_t el[30] = {0, 0, 0, 0, 0, 3, 1, 8, 0, 1, 0, 4, 1, 8, 1, 1, 0, 4, 1, 8, 2, 1, 0, 4, 1, 8, 3, 0, 8, 4};
        const vkn_vfmt *f = vkn_vfmt_info(vkn_vfmt_put(H(802, 1), el, 5)); CHECK(f != NULL);
        if (f) {
            for (uint32_t i = 0; i < 3; i++) { CHECK_EQ(attr_at(f, 9 + i)->format, VK_FORMAT_R32G32B32A32_SFLOAT); CHECK_EQ(attr_at(f, 9 + i)->offset, 16 * i); CHECK_EQ(attr_at(f, 9 + i)->binding, 1); }
            CHECK_EQ(attr_at(f, 12)->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(attr_at(f, 12)->offset, 48);
            CHECK_EQ(bind_at(f, 1)->stride, 52); CHECK_EQ(bind_at(f, 1)->inputRate, VK_VERTEX_INPUT_RATE_INSTANCE);
            CHECK_EQ(bind_at(f, 0)->stride, 12); CHECK_EQ(bind_at(f, 0)->inputRate, VK_VERTEX_INPUT_RATE_VERTEX);
        }
    }
    // 0x9181d0 edge rules: a type beyond 11 adds nothing; a count beyond 4 adds size x count but no attribute; a location set twice keeps
    // the last element; a stream beyond 2 is ignored
    {
        const uint8_t el[36] = {0, 0, 0, 0, 12, 4, 0, 2, 0, 0, 0, 5, 0, 2, 1, 0, 0, 2, 0, 3, 0, 0, 0, 1, 0, 3, 0, 0, 1, 2, 3, 6, 0, 0, 0, 4};
        const vkn_vfmt *f = vkn_vfmt_info(vkn_vfmt_put(H(803, 1), el, 6)); CHECK(f != NULL);
        if (f) {
            CHECK_EQ(f->nattr, 2); CHECK_EQ(attr_at(f, 5)->offset, 20); CHECK_EQ(attr_at(f, 5)->format, VK_FORMAT_R32G32_SFLOAT);
            CHECK_EQ(attr_at(f, 1)->format, VK_FORMAT_R32G32_UINT); CHECK_EQ(attr_at(f, 1)->offset, 32); CHECK_EQ(f->loc_class[1], 3); CHECK_EQ(f->loc_comps[1], 2);
            CHECK_EQ(f->stride[0], 40); CHECK_EQ(bind_at(f, 0)->stride, 40); CHECK(attr_at(f, 14) == NULL); CHECK_EQ(f->nbind, 1);
        }
    }
    // 3-component 8 / 16-bit formats a device lacks as vertex formats: read as 4 components
    mk_vtx3 = 0;
    {
        const uint8_t el[18] = {0, 0, 0, 0, 8, 3, 0, 2, 0, 0, 3, 3, 0, 3, 0, 0, 0, 3};
        const vkn_vfmt *f = vkn_vfmt_info(vkn_vfmt_put(H(804, 1), el, 3)); CHECK(f != NULL);
        if (f) {
            CHECK_EQ(attr_at(f, 0)->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(attr_at(f, 4)->format, VK_FORMAT_R16G16B16A16_SFLOAT);
            CHECK_EQ(attr_at(f, 1)->format, VK_FORMAT_R32G32B32_SFLOAT);
            CHECK_EQ(f->widened, (1u << 0) | (1u << 4)); CHECK_EQ(f->loc_comps[0], 3);
            CHECK_EQ(attr_at(f, 4)->offset, 3); CHECK_EQ(attr_at(f, 1)->offset, 9); CHECK_EQ(f->stride[0], 21);   // offsets keep the engine's sizes
        }
    }
    mk_vtx3 = 1;
    // dedup and kill
    CHECK_EQ(vkn_vfmt_put(H(805, 1), mesh, 5), mesh_id); CHECK_EQ(vkn_vfmt_id(H(805, 1)), mesh_id); CHECK_EQ(vkn_vfmt_id(H(801, 1)), mesh_id);
    CHECK(vkn_vfmt_put(H(806, 1), mesh, 4) != mesh_id);
    CHECK_EQ(vkn_vfmt_kill(H(805, 1)), 1); CHECK_EQ(vkn_vfmt_id(H(805, 1)), 0); CHECK(vkn_vfmt_info(mesh_id) != NULL);
    CHECK_EQ(vkn_vfmt_put(H(807, 1), mesh, 256), 0); CHECK_EQ(vkn_vfmt_put(H(808, 1), NULL, 2), 0);
    const uint16_t empty = vkn_vfmt_put(H(809, 1), NULL, 0); CHECK(empty != 0); CHECK(vkn_vfmt_info(empty) && vkn_vfmt_info(empty)->nattr == 0);
}

static void test_masks_prims(void) {
    for (uint32_t n = 0; n < 0x100; n++) CHECK_EQ(vkn_write_mask_nvn(n), exp_mask(n));
    for (uint32_t e = 0; e < 70; e++) {
        const uint32_t nvn = e < 63 ? nr_formats[e].nvn : 0, srgb = e < 63 ? nr_formats[e].nvn_srgb : 0;
        CHECK_EQ(vkn_write_mask_engine(e), nvn == NR_NVN_TRAP ? 0 : exp_mask(nvn));
        if (e < 63 && srgb != NR_NVN_TRAP) CHECK_EQ(exp_mask(srgb), exp_mask(nvn == NR_NVN_TRAP ? 0 : nvn));   // sRGB does not change the mask
    }
    // primitives: engine 1..7 -> NVN by the table at main.raw 0x1f58640 = {4, 6, 5, 7, 1, 3, 2}; out of range -> NVN 0 (points)
    static const uint8_t nvn_of[8] = {0, 4, 6, 5, 7, 1, 3, 2};
    for (uint32_t p = 0; p < 12; p++) {
        const uint8_t np = p >= 1 && p <= 7 ? nvn_of[p] : 0;   // NVN: 0 points, 1 lines, 2 line loop, 3 line strip, 4 triangles, 5 strip, 6 fan, 7 quads
        VkPrimitiveTopology t = (VkPrimitiveTopology)99; const int rw = vkn_prim_topology((uint8_t)p, &t);
        CHECK_EQ(rw, np == 2 || np == 6 || np == 7);
        if (!rw) CHECK_EQ(t, np == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : np == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : np == 3 ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                               : np == 4 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
        else CHECK_EQ(t, 99);
        const uint8_t q = vkn_prim_rewrite((uint8_t)p), nq = q >= 1 && q <= 7 ? nvn_of[q] : 0;
        if (rw) CHECK_EQ(nq, np == 2 ? 3 : 4);   // line loop -> line strip (plus the first index again); quads, fan -> triangles
        else CHECK_EQ(q, p);
        VkPrimitiveTopology t2; CHECK_EQ(vkn_prim_topology(q, &t2), 0);
    }
}

static VkPipelineLayout g_pl;   // a pipeline layout of the test's own (one empty set)
static vkn_tex *rt(uint32_t efmt, uint32_t cls, uint32_t w, uint32_t h, uint64_t cookie) {
    vkn_tex_desc d = tdesc(efmt, 1, w, h, 1, 1, cls, cookie, ~0u); return vkn_tex_create(H(1500 + (cookie & 0xff), 1), &d, NULL, NULL);
}
static void test_passes(void) {
    vkn_rp_key k; vkn_rp_key_init(&k);
    CHECK_EQ(k.samples, 1); CHECK_EQ(k.ncolor, 0);
    k.ncolor = 3; k.color[0] = VK_FORMAT_R8G8B8A8_UNORM; k.color[2] = VK_FORMAT_R16G16B16A16_SFLOAT; k.depth = VK_FORMAT_D24_UNORM_S8_UINT;
    k.load[0] = VK_ATTACHMENT_LOAD_OP_LOAD; k.store[0] = VK_ATTACHMENT_STORE_OP_STORE; k.load[2] = VK_ATTACHMENT_LOAD_OP_CLEAR; k.store[2] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    k.depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR; k.depth_store = VK_ATTACHMENT_STORE_OP_STORE; k.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE; k.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    const uint64_t rp0 = mk_created[MK_RP];
    VkRenderPass p1 = VK_NULL_HANDLE; uint16_t c1 = 0;
    const uint16_t id1 = vkn_rp_get(&k, &p1, &c1);
    CHECK(id1 && p1 && c1); CHECK_EQ(mk_created[MK_RP], rp0 + 1);
    mk_obj *o = mk_obj_of((uint64_t)(uintptr_t)p1);
    if (o) {
        const mk_rp *r = &o->rp;
        CHECK_EQ(r->natt, 3);
        CHECK_EQ(r->att[0].format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(r->att[0].loadOp, VK_ATTACHMENT_LOAD_OP_LOAD); CHECK_EQ(r->att[0].storeOp, VK_ATTACHMENT_STORE_OP_STORE);
        CHECK_EQ(r->att[0].initialLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); CHECK_EQ(r->att[0].finalLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK_EQ(r->att[0].samples, VK_SAMPLE_COUNT_1_BIT); CHECK_EQ(r->att[0].stencilLoadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        CHECK_EQ(r->att[1].format, VK_FORMAT_R16G16B16A16_SFLOAT); CHECK_EQ(r->att[1].loadOp, VK_ATTACHMENT_LOAD_OP_CLEAR); CHECK_EQ(r->att[1].storeOp, VK_ATTACHMENT_STORE_OP_DONT_CARE);
        CHECK_EQ(r->att[1].initialLayout, VK_IMAGE_LAYOUT_UNDEFINED); CHECK_EQ(r->att[1].finalLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK_EQ(r->att[2].format, VK_FORMAT_D24_UNORM_S8_UINT); CHECK_EQ(r->att[2].loadOp, VK_ATTACHMENT_LOAD_OP_CLEAR); CHECK_EQ(r->att[2].storeOp, VK_ATTACHMENT_STORE_OP_STORE);
        CHECK_EQ(r->att[2].stencilLoadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE); CHECK_EQ(r->att[2].stencilStoreOp, VK_ATTACHMENT_STORE_OP_DONT_CARE);
        CHECK_EQ(r->att[2].initialLayout, VK_IMAGE_LAYOUT_UNDEFINED); CHECK_EQ(r->att[2].finalLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        CHECK_EQ(r->ncolor, 3);
        CHECK_EQ(r->color[0].attachment, 0); CHECK_EQ(r->color[0].layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK_EQ(r->color[1].attachment, VK_ATTACHMENT_UNUSED); CHECK_EQ(r->color[2].attachment, 1);
        CHECK_EQ(r->has_depth, 1); CHECK_EQ(r->depth.attachment, 2); CHECK_EQ(r->depth.layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        CHECK_EQ(r->ndep, 2); CHECK_EQ(r->dep[0].srcSubpass, VK_SUBPASS_EXTERNAL); CHECK_EQ(r->dep[0].dstSubpass, 0); CHECK_EQ(r->dep[1].srcSubpass, 0); CHECK_EQ(r->dep[1].dstSubpass, VK_SUBPASS_EXTERNAL);
    }
    // the same key -> the same pass; what cannot matter (an unbound slot's ops, trailing unbound slots) does not make a new one
    VkRenderPass p; uint16_t c;
    CHECK_EQ(vkn_rp_get(&k, &p, &c), id1); CHECK(p == p1 && c == c1);
    vkn_rp_key k2 = k; k2.load[1] = VK_ATTACHMENT_LOAD_OP_LOAD; k2.store[1] = 1; k2.ncolor = 5; k2.load[4] = 2; k2.pad[0] = 9;
    CHECK_EQ(vkn_rp_get(&k2, &p, &c), id1); CHECK_EQ(mk_created[MK_RP], rp0 + 1);
    // other load ops: a new pass, the same compatibility class (its first pass is what pipelines are built against)
    k2 = k; k2.load[0] = VK_ATTACHMENT_LOAD_OP_CLEAR;
    const uint16_t id2 = vkn_rp_get(&k2, &p, &c);
    CHECK(id2 && id2 != id1 && p != p1); CHECK_EQ(c, c1); CHECK(vkn_rp_compat_pass(c1) == p1);
    o = mk_obj_of((uint64_t)(uintptr_t)p); if (o) CHECK_EQ(o->rp.att[0].initialLayout, VK_IMAGE_LAYOUT_UNDEFINED);
    // depth loaded -> initial layout DEPTH_STENCIL_ATTACHMENT_OPTIMAL (stencil load counts too)
    k2 = k; k2.stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    vkn_rp_get(&k2, &p, &c); o = mk_obj_of((uint64_t)(uintptr_t)p); if (o) { CHECK_EQ(o->rp.att[2].initialLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL); CHECK_EQ(o->rp.att[2].stencilLoadOp, VK_ATTACHMENT_LOAD_OP_LOAD); }
    CHECK_EQ(c, c1);
    // a depth format without stencil: stencil ops do not matter (one pass for both), a new class
    vkn_rp_key k3 = k; k3.depth = VK_FORMAT_D32_SFLOAT; k3.stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    uint16_t c3; const uint16_t id3 = vkn_rp_get(&k3, &p, &c3);
    CHECK(id3 && c3 != c1);
    o = mk_obj_of((uint64_t)(uintptr_t)p); if (o) { CHECK_EQ(o->rp.att[2].stencilLoadOp, VK_ATTACHMENT_LOAD_OP_DONT_CARE); CHECK_EQ(o->rp.att[2].initialLayout, VK_IMAGE_LAYOUT_UNDEFINED); }
    k3.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR; k3.stencil_store = VK_ATTACHMENT_STORE_OP_STORE; CHECK_EQ(vkn_rp_get(&k3, NULL, NULL), id3);
    // samples: a class of its own
    vkn_rp_key k4 = k; k4.samples = 4; uint16_t c4; vkn_rp_get(&k4, &p, &c4); CHECK(c4 != c1 && c4 != c3);
    o = mk_obj_of((uint64_t)(uintptr_t)p); if (o) { CHECK_EQ(o->rp.att[0].samples, VK_SAMPLE_COUNT_4_BIT); CHECK_EQ(o->rp.att[2].samples, VK_SAMPLE_COUNT_4_BIT); }
    // nothing bound
    vkn_rp_key k5; vkn_rp_key_init(&k5); CHECK(vkn_rp_get(&k5, &p, &c) != 0); o = mk_obj_of((uint64_t)(uintptr_t)p); if (o) { CHECK_EQ(o->rp.natt, 0); CHECK_EQ(o->rp.has_depth, 0); CHECK_EQ(o->rp.ncolor, 0); }
    vkn_state_stats ss; vkn_state_stats_get(&ss); CHECK_EQ(ss.compat, 4); CHECK_EQ(ss.passes, 6);

    // framebuffers of the class c1: views of real targets
    vkn_tex *t0 = rt(4, VKN_TC_COLOR, 64, 32, 0xFB0), *t2 = rt(20, VKN_TC_COLOR, 64, 32, 0xFB2), *td = rt(51, VKN_TC_DEPTH, 64, 32, 0xFBD);
    CHECK(t0 && t2 && td); if (!t0 || !t2 || !td) return;
    const VkImageView v3[3] = {t0->att, t2->att, td->att};
    const uint64_t fb0 = mk_created[MK_FB];
    const VkFramebuffer f1 = vkn_fb_get(c1, v3, 3, 64, 32, 1);
    CHECK(f1 != VK_NULL_HANDLE); CHECK_EQ(mk_created[MK_FB], fb0 + 1);
    o = mk_obj_of((uint64_t)(uintptr_t)f1);
    if (o) { CHECK(o->fb_pass == p1); CHECK_EQ(o->fb_n, 3); CHECK(o->fb_view[0] == v3[0] && o->fb_view[2] == v3[2]); CHECK_EQ(o->fb_w, 64); CHECK_EQ(o->fb_h, 32); CHECK_EQ(o->fb_layers, 1); }
    CHECK(vkn_fb_get(c1, v3, 3, 64, 32, 1) == f1); CHECK(vkn_fb_get(c1, v3, 3, 64, 32, 0) == f1); CHECK_EQ(mk_created[MK_FB], fb0 + 1);
    const VkFramebuffer f2 = vkn_fb_get(c1, v3, 3, 32, 32, 1);
    CHECK(f2 && f2 != f1);
    CHECK(vkn_fb_get(c1, v3, 13, 64, 32, 1) == VK_NULL_HANDLE); CHECK(vkn_fb_get(c1, v3, 3, 0, 32, 1) == VK_NULL_HANDLE); CHECK(vkn_fb_get(200, v3, 3, 64, 32, 1) == VK_NULL_HANDLE);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.framebuffers, 2);
    const VkImageView v3b[3] = {t2->att, t0->att, td->att};   // other views of the same class: another framebuffer, untouched by the forget below
    const VkFramebuffer f3 = vkn_fb_get(c1, v3b, 3, 64, 32, 1); CHECK(f3 && f3 != f1);
    const VkImageView only_t2[3] = {t2->att, t2->att, td->att};
    const VkFramebuffer f4 = vkn_fb_get(c1, only_t2, 3, 64, 32, 1); CHECK(f4 != VK_NULL_HANDLE);
    // forget a view: every framebuffer with it is retired (destroyed 3 frame ends later) and leaves the cache
    vkn_fb_forget_view(t0->att);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.framebuffers, 1);
    frames(2); CHECK(mk_is_live((uint64_t)(uintptr_t)f1, MK_FB)); frames(1);
    CHECK(!mk_is_live((uint64_t)(uintptr_t)f1, MK_FB)); CHECK(!mk_is_live((uint64_t)(uintptr_t)f2, MK_FB)); CHECK(!mk_is_live((uint64_t)(uintptr_t)f3, MK_FB));
    CHECK(mk_is_live((uint64_t)(uintptr_t)f4, MK_FB));
    const VkFramebuffer f5 = vkn_fb_get(c1, v3, 3, 64, 32, 1); CHECK(f5 && f5 != f1); CHECK_EQ(mk_created[MK_FB], fb0 + 5);
    // [VK-DRV] DM_VK_NATIVE_FB_EXACT (PowerVR stock): a framebuffer made with the exact pass - another pass of the class has its own
    {vkn_rp_key kx = k; kx.load[0] = k.load[0] == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
     VkRenderPass px = VK_NULL_HANDLE; uint16_t cx = 0; vkn_rp_get(&kx, &px, &cx); CHECK(cx == c1 && px && px != p1);
     const VkFramebuffer fx = vkn_fb_get_rp(c1, px, v3, 3, 64, 32, 1); CHECK(fx && fx != f5);
     o = mk_obj_of((uint64_t)(uintptr_t)fx); if (o) CHECK(o->fb_pass == px);
     CHECK(vkn_fb_get_rp(c1, px, v3, 3, 64, 32, 1) == fx); CHECK(vkn_fb_get_rp(c1, VK_NULL_HANDLE, v3, 3, 64, 32, 1) == f5);
     vkn_fb_forget_view(t0->att); frames(3); CHECK(!mk_is_live((uint64_t)(uintptr_t)fx, MK_FB));
     const VkFramebuffer f5b = vkn_fb_get(c1, v3, 3, 64, 32, 1); CHECK(f5b != VK_NULL_HANDLE);}
    // the texture piece forgets the framebuffers of a texture's views when the engine destroys it (vkn_state.h: before the view dies)
    vkn_tex_destroy_cookie(0xFB2);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.framebuffers, 0);
    frames(3); CHECK(!mk_is_live((uint64_t)(uintptr_t)f4, MK_FB)); CHECK(!mk_is_live((uint64_t)(uintptr_t)f5, MK_FB));
    vkn_tex_destroy_cookie(0xFB0); vkn_tex_destroy_cookie(0xFBD);
    frames(3);
    CHECK_EQ(mk_live(MK_FB), 0);
}

static void dead_to_pipes(uint32_t id, uint64_t handle) { (void)handle; vkn_pipe_forget_shader(id); }   // the wiring the host must do
static uint16_t g_c1;   // the class of test_passes' first pass (RGBA8, -, RGBA16F, D24S8)

static void test_pipelines(void) {
    VkDescriptorSetLayout sl; const VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    vkCreateDescriptorSetLayout(MK_DEV, &li, NULL, &sl);
    const VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &sl};
    vkCreatePipelineLayout(MK_DEV, &pli, NULL, &g_pl);
    vkn_rp_key k; vkn_rp_key_init(&k);
    k.ncolor = 3; k.color[0] = VK_FORMAT_R8G8B8A8_UNORM; k.color[2] = VK_FORMAT_R16G16B16A16_SFLOAT; k.depth = VK_FORMAT_D24_UNORM_S8_UINT;
    k.load[0] = VK_ATTACHMENT_LOAD_OP_LOAD; k.store[0] = VK_ATTACHMENT_STORE_OP_STORE; k.load[2] = VK_ATTACHMENT_LOAD_OP_CLEAR; k.store[2] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    k.depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR; k.depth_store = VK_ATTACHMENT_STORE_OP_STORE; k.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE; k.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderPass p1; vkn_rp_get(&k, &p1, &g_c1);
    const uint16_t blend = vkn_blend_put(H(900, 1), 6, 7, 0, 0, 0, 0), ds = vkn_ds_put(H(900, 1), 1, 1, 3, 1, 0xff, 7, 0, 2), ds_nost = vkn_ds_put(H(901, 1), 1, 0, 1, 0, 0xff, 0, 0, 0);
    const uint16_t raster = vkn_raster_put(H(900, 1), 1, 0, 0, 0, 1);
    const uint8_t mesh[30] = {0, 0, 0, 0, 0, 3, 0, 3, 0, 0, 8, 4, 0, 2, 0, 0, 3, 2, 0, 2, 1, 0, 3, 2, 1, 4, 0, 0, 10, 4};
    const uint16_t vfmt = vkn_vfmt_put(H(900, 1), mesh, 5);
    // the key: pack / unpack
    vkn_pipe_desc d; memset(&d, 0, sizeof d);
    d.shader = 77; d.vs_variant = 1; d.fs_variant = 2; d.blend = blend; d.ds = ds; d.raster = raster; d.vfmt = vfmt; d.prim = 1; d.stencil_ref = 0x33; d.pass = g_c1;
    d.write_mask[0] = 0xf; d.write_mask[1] = 0xf; d.write_mask[2] = 0x7; d.write_mask[5] = 0x3;
    const vkn_pipe_key key = vkn_pipe_key_pack(&d);
    vkn_pipe_desc u; vkn_pipe_key_unpack(&key, &u);
    CHECK_EQ(u.shader, 77); CHECK_EQ(u.vs_variant, 1); CHECK_EQ(u.fs_variant, 2); CHECK_EQ(u.blend, blend); CHECK_EQ(u.ds, ds); CHECK_EQ(u.raster, raster); CHECK_EQ(u.vfmt, vfmt);
    CHECK_EQ(u.prim, 1); CHECK_EQ(u.stencil_ref, 0x33); CHECK_EQ(u.pass, g_c1);
    CHECK_EQ(u.write_mask[0], 0xf); CHECK_EQ(u.write_mask[1], 0); CHECK_EQ(u.write_mask[2], 0x7); CHECK_EQ(u.write_mask[5], 0);   // unbound slots: no mask
    {   // the reference only with a stencil test on a pass with stencil
        vkn_pipe_desc e = d; e.ds = ds_nost; vkn_pipe_key ke = vkn_pipe_key_pack(&e); vkn_pipe_key_unpack(&ke, &u); CHECK_EQ(u.stencil_ref, 0);
        e = d; e.ds = 0; ke = vkn_pipe_key_pack(&e); vkn_pipe_key_unpack(&ke, &u); CHECK_EQ(u.stencil_ref, 0);
    }
    // round trip of random descriptors (no pass: masks all kept, reference dropped); every field changes the key
    uint32_t rs = 12345;
    for (int i = 0; i < 2000; i++) {
        vkn_pipe_desc r; memset(&r, 0, sizeof r);
#define RND() (rs = rs * 1103515245u + 12345u, rs >> 8)
        r.shader = RND() & 0xffffff; r.vs_variant = (uint8_t)RND(); r.fs_variant = (uint8_t)RND(); r.blend = (uint16_t)RND(); r.ds = (uint16_t)RND();
        r.raster = (uint16_t)RND(); r.vfmt = (uint16_t)RND(); r.prim = (uint8_t)RND();
        for (int s = 0; s < 8; s++) r.write_mask[s] = (uint8_t)(RND() & 0xf);
        const vkn_pipe_key rk = vkn_pipe_key_pack(&r); vkn_pipe_desc ru; vkn_pipe_key_unpack(&rk, &ru);
        CHECK(!memcmp(&r, &ru, sizeof r));
        vkn_pipe_desc m = r; const int fld = i % 10;
        switch (fld) { case 0: m.shader ^= 1u << (i % 24); break; case 1: m.vs_variant ^= 1; break; case 2: m.fs_variant ^= 0x80; break; case 3: m.blend ^= 0x8000; break;
                       case 4: m.ds ^= 1; break; case 5: m.raster ^= 0x100; break; case 6: m.vfmt ^= 2; break; case 7: m.prim ^= 0x40; break; case 8: m.write_mask[7] ^= 8; break; default: m.pass ^= 1; }
        if (fld == 9) continue;   // (a pass id changes masks too; covered above)
        const vkn_pipe_key mk = vkn_pipe_key_pack(&m); CHECK(memcmp(&mk, &rk, sizeof mk) != 0);
#undef RND
    }
    // the cache: a lookup without sources never builds; a miss builds; the same key hits
    vkn_state_stats ss; vkn_state_stats_get(&ss); const uint64_t miss0 = ss.misses, hit0 = ss.hits;
    const uint64_t pp0 = mk_created[MK_PIPE];
    CHECK(vkn_pipe_get(&key, NULL) == VK_NULL_HANDLE); CHECK_EQ(mk_created[MK_PIPE], pp0);
    const VkShaderModule M1 = (VkShaderModule)(uintptr_t)0x1111000ull, M2 = (VkShaderModule)(uintptr_t)0x2222000ull;
    vkn_pipe_src src = {M1, M2, g_pl, VK_NULL_HANDLE, (uint16_t)0x3f};   // the vertex module reads locations 0..5; the format has no 2
    const VkPipeline P = vkn_pipe_get(&key, &src);
    CHECK(P != VK_NULL_HANDLE); CHECK_EQ(mk_created[MK_PIPE], pp0 + 1);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.misses, miss0 + 1);
    mk_obj *o = mk_obj_of((uint64_t)(uintptr_t)P);
    if (o) {
        const mk_gp *g = &o->gp;
        CHECK_EQ(g->nst, 2); CHECK_EQ(g->stage[0], VK_SHADER_STAGE_VERTEX_BIT); CHECK(g->module[0] == M1); CHECK_EQ(g->stage[1], VK_SHADER_STAGE_FRAGMENT_BIT); CHECK(g->module[1] == M2);
        CHECK(!strcmp(g->entry[0], "main"));
        const vkn_vfmt *vf = vkn_vfmt_info(vfmt);
        CHECK_EQ(g->nattr, vf->nattr + 1); CHECK_EQ(g->nbind, vf->nbind + 1);
        int null_ok = 0, attrs_ok = 1;
        for (uint32_t i = 0; i < g->nattr; i++) {
            if (g->attr[i].location == 2) null_ok = g->attr[i].binding == VKN_VFMT_NULL_BINDING && g->attr[i].format == VK_FORMAT_R32G32B32A32_SFLOAT && g->attr[i].offset == 0;
            else { const VkVertexInputAttributeDescription *a = attr_at(vf, g->attr[i].location); attrs_ok &= a && !memcmp(a, &g->attr[i], sizeof *a); }
        }
        CHECK(null_ok); CHECK(attrs_ok);
        int nb_ok = 0; for (uint32_t i = 0; i < g->nbind; i++) if (g->bind[i].binding == VKN_VFMT_NULL_BINDING) nb_ok = g->bind[i].stride == 0 && g->bind[i].inputRate == VK_VERTEX_INPUT_RATE_VERTEX;
        CHECK(nb_ok);
        CHECK_EQ(g->topo, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); CHECK_EQ(g->restart, 0); CHECK_EQ(g->nvp, 1); CHECK_EQ(g->nsc, 1);
        CHECK_EQ(g->rs.cullMode, VK_CULL_MODE_BACK_BIT); CHECK_EQ(g->rs.polygonMode, VK_POLYGON_MODE_FILL); CHECK_EQ(g->rs.frontFace, VK_FRONT_FACE_CLOCKWISE);
        CHECK_EQ(g->rs.depthBiasEnable, 1); CHECK(g->rs.depthBiasSlopeFactor == -2.5f && g->rs.depthBiasConstantFactor == -5.0f); CHECK(g->rs.lineWidth == 1.0f);
        CHECK_EQ(g->samples, VK_SAMPLE_COUNT_1_BIT);
        CHECK_EQ(g->has_ds, 1); CHECK_EQ(g->ds.depthTestEnable, 1); CHECK_EQ(g->ds.depthWriteEnable, 1); CHECK_EQ(g->ds.depthCompareOp, VK_COMPARE_OP_LESS_OR_EQUAL);
        CHECK_EQ(g->ds.stencilTestEnable, 1); CHECK_EQ(g->ds.front.reference, 0x33); CHECK_EQ(g->ds.back.reference, 0x33); CHECK_EQ(g->ds.front.compareOp, VK_COMPARE_OP_ALWAYS);
        CHECK_EQ(g->ds.front.passOp, VK_STENCIL_OP_REPLACE); CHECK_EQ(g->ds.front.compareMask, 0xff);
        CHECK_EQ(g->natt, 3);
        CHECK_EQ(g->att[0].blendEnable, 1); CHECK_EQ(g->att[0].srcColorBlendFactor, VK_BLEND_FACTOR_SRC_ALPHA); CHECK_EQ(g->att[0].colorWriteMask, 0xf);
        CHECK_EQ(g->att[1].blendEnable, 0); CHECK_EQ(g->att[1].colorWriteMask, 0);
        CHECK_EQ(g->att[2].blendEnable, 1); CHECK_EQ(g->att[2].colorWriteMask, 0x7);
        CHECK_EQ(g->ndyn, 2); CHECK(g->dyn[0] == VK_DYNAMIC_STATE_VIEWPORT && g->dyn[1] == VK_DYNAMIC_STATE_SCISSOR);
        CHECK(g->layout == g_pl); CHECK(g->pass == vkn_rp_compat_pass(g_c1)); CHECK_EQ(g->subpass, 0); CHECK(g->cache != VK_NULL_HANDLE);
    }
    CHECK(vkn_pipe_get(&key, &src) == P); CHECK(vkn_pipe_get(&key, NULL) == P); CHECK_EQ(mk_created[MK_PIPE], pp0 + 1);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.hits, hit0 + 2);
    // variations
    vkn_pipe_desc e = d; e.stencil_ref = 0x34; vkn_pipe_key ke = vkn_pipe_key_pack(&e);
    const VkPipeline P2 = vkn_pipe_get(&ke, &src); CHECK(P2 && P2 != P);
    e = d; e.raster = 0; e.ds = 0; e.blend = 0; e.vfmt = 0; ke = vkn_pipe_key_pack(&e);
    vkn_pipe_src s0 = {M1, VK_NULL_HANDLE, g_pl, VK_NULL_HANDLE, 0};
    const VkPipeline P3 = vkn_pipe_get(&ke, &s0); CHECK(P3 != VK_NULL_HANDLE);
    o = mk_obj_of((uint64_t)(uintptr_t)P3);
    if (o) {
        const mk_gp *g = &o->gp;
        CHECK_EQ(g->nst, 1); CHECK_EQ(g->nattr, 0); CHECK_EQ(g->nbind, 0); CHECK_EQ(g->rs.cullMode, VK_CULL_MODE_NONE); CHECK_EQ(g->rs.depthBiasEnable, 0);
        CHECK_EQ(g->ds.depthTestEnable, 0); CHECK_EQ(g->ds.stencilTestEnable, 0); CHECK_EQ(g->att[0].blendEnable, 0); CHECK_EQ(g->att[0].colorWriteMask, 0xf);
    }
    {   // a pass without depth: no depth-stencil state; a D32F pass: no stencil test; an integer target: no blending; 4 samples
        vkn_rp_key kn; vkn_rp_key_init(&kn); kn.ncolor = 1; kn.color[0] = VK_FORMAT_R8G8B8A8_UINT; kn.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
        uint16_t cn; vkn_rp_get(&kn, NULL, &cn);
        e = d; e.pass = cn; ke = vkn_pipe_key_pack(&e); o = mk_obj_of((uint64_t)(uintptr_t)vkn_pipe_get(&ke, &src));
        CHECK(o && !o->gp.has_ds && o->gp.natt == 1 && !o->gp.att[0].blendEnable && o->gp.att[0].colorWriteMask == 0xf);
        vkn_rp_key kz = k; kz.depth = VK_FORMAT_D32_SFLOAT; kz.samples = 4; uint16_t cz; vkn_rp_get(&kz, NULL, &cz);
        e = d; e.pass = cz; ke = vkn_pipe_key_pack(&e); vkn_pipe_key_unpack(&ke, &u); CHECK_EQ(u.stencil_ref, 0);
        o = mk_obj_of((uint64_t)(uintptr_t)vkn_pipe_get(&ke, &src));
        CHECK(o && o->gp.has_ds && !o->gp.ds.stencilTestEnable && o->gp.ds.depthTestEnable && o->gp.samples == VK_SAMPLE_COUNT_4_BIT);
        // fill mode lines (the device has fillModeNonSolid), points for an out-of-range primitive
        e = d; e.raster = vkn_raster_put(H(902, 1), 0, 1, 0, 0, 0); e.prim = 9; ke = vkn_pipe_key_pack(&e); o = mk_obj_of((uint64_t)(uintptr_t)vkn_pipe_get(&ke, &src));
        CHECK(o && o->gp.rs.polygonMode == VK_POLYGON_MODE_LINE && o->gp.topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    }
    // a primitive that needs an index rewrite: nothing built, counted, not cached
    e = d; e.prim = 4; ke = vkn_pipe_key_pack(&e);
    vkn_state_stats_get(&ss); const uint64_t rw0 = ss.rewrites, n0 = mk_created[MK_PIPE];
    CHECK(vkn_pipe_get(&ke, &src) == VK_NULL_HANDLE); CHECK(vkn_pipe_get(&ke, &src) == VK_NULL_HANDLE);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.rewrites, rw0 + 2); CHECK_EQ(mk_created[MK_PIPE], n0);
    // a failed build is cached (no rebuild storm)
    e = d; e.shader = 79; ke = vkn_pipe_key_pack(&e);
    vkn_pipe_src bad = {MK_BAD_MODULE, M2, g_pl, VK_NULL_HANDLE, 0};
    vkn_state_stats_get(&ss); const uint64_t f0 = ss.failures; const int fc0 = mk_pipe_fail_calls;
    CHECK(vkn_pipe_get(&ke, &bad) == VK_NULL_HANDLE); CHECK(vkn_pipe_get(&ke, &bad) == VK_NULL_HANDLE); CHECK(vkn_pipe_get(&ke, &src) == VK_NULL_HANDLE);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.failures, f0 + 1); CHECK_EQ(mk_pipe_fail_calls, fc0 + 1);
    // compute pipelines
    const VkShaderModule M3 = (VkShaderModule)(uintptr_t)0x3333000ull;
    const VkPipeline C = vkn_pipe_compute(78, M3, g_pl);
    CHECK(C != VK_NULL_HANDLE); o = mk_obj_of((uint64_t)(uintptr_t)C); CHECK(o && o->compute && o->cs_module == M3 && o->cs_layout == g_pl);
    CHECK(vkn_pipe_compute(78, M3, g_pl) == C); CHECK(vkn_pipe_compute(78, VK_NULL_HANDLE, VK_NULL_HANDLE) == C);
    CHECK(vkn_pipe_compute(80, VK_NULL_HANDLE, g_pl) == VK_NULL_HANDLE);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.compute, 1);
    // a shader id dies: its graphics and compute pipelines are retired, a later get builds again
    const uint64_t live0 = mk_live(MK_PIPE);
    vkn_pipe_forget_shader(77);
    frames(2); CHECK(mk_is_live((uint64_t)(uintptr_t)P, MK_PIPE)); frames(1); CHECK(!mk_is_live((uint64_t)(uintptr_t)P, MK_PIPE)); CHECK(!mk_is_live((uint64_t)(uintptr_t)P2, MK_PIPE));
    CHECK(mk_live(MK_PIPE) < live0); CHECK(mk_is_live((uint64_t)(uintptr_t)C, MK_PIPE));
    const VkPipeline P4 = vkn_pipe_get(&key, &src); CHECK(P4 && P4 != P);
    vkn_pipe_forget_shader(78); frames(3); CHECK(!mk_is_live((uint64_t)(uintptr_t)C, MK_PIPE));
    vkn_state_stats_get(&ss); CHECK_EQ(ss.compute, 0);
    vkn_pipe_forget_shader(77); vkn_pipe_forget_shader(79); frames(3);
    CHECK_EQ(mk_live(MK_PIPE), 0);
    vkn_state_stats_get(&ss); CHECK_EQ(ss.pipelines, 0);
}

static void test_state(void) {
    test_state_objects();
    test_vfmt();
    test_masks_prims();
    test_passes();
    test_pipelines();
}
// ================================================================================================================ shaders
enum { CTL = 0xe48 };
static uint8_t g_ctl[16][CTL], g_code[16][64];
static vkn_stage_src stage(int i, const char *key, uint32_t mask) {   // a stage whose control section carries its program key
    memset(g_ctl[i], 0, CTL); memcpy(g_ctl[i], key, 16); memset(g_code[i], i, 64);
    return (vkn_stage_src){g_ctl[i], g_code[i], "shaders/test.bshd", mask};
}
static uint32_t g_dead_id[16]; static uint64_t g_dead_h[16]; static int g_ndead;
static void dead_rec(uint32_t id, uint64_t h) { if (g_ndead < 16) { g_dead_id[g_ndead] = id; g_dead_h[g_ndead] = h; g_ndead++; } dead_to_pipes(id, h); }
static void set_dyn_limit(uint32_t n) { ((vkn_dev *)vkn_device())->props.limits.maxDescriptorSetUniformBuffersDynamic = n; }   // what the device reported
typedef struct { uint16_t binding; uint8_t kind, stage, slot, dyn, bundle_stage; VkDescriptorType type; } exp_bind;
static void check_table(const vkn_shader *s, const exp_bind *e, uint32_t n, const char *what) {
    CHECK_EQ(s->nbind, n);
    if (s->nbind != n) { printf("  (%s)\n", what); return; }
    for (uint32_t i = 0; i < n; i++) {
        const vkn_bind *b = &s->bind[i];
        CHECK_EQ(b->binding, e[i].binding); CHECK_EQ(b->kind, e[i].kind); CHECK_EQ(b->stage, e[i].stage); CHECK_EQ(b->slot, e[i].slot);
        CHECK_EQ(b->dyn, e[i].dyn); CHECK_EQ(b->bundle_stage, e[i].bundle_stage); CHECK_EQ(b->type, e[i].type); CHECK_EQ(b->count, 1);
        CHECK_EQ(b->binding % 6, b->bundle_stage);   // the packs' rule: Vulkan binding = original binding x 6 + bundle stage
    }
    // the set layout: the same bindings, uniform buffers as the table says, one descriptor each, the bundle stage's shader stage
    mk_obj *l = mk_obj_of((uint64_t)(uintptr_t)s->set_layout); CHECK(l && l->kind == MK_DSL && l->live); if (!l) return;
    CHECK_EQ(l->nb, n);
    for (uint32_t i = 0; i < n && i < l->nb; i++) {
        CHECK_EQ(l->b[i].binding, e[i].binding); CHECK_EQ(l->b[i].descriptorType, e[i].type); CHECK_EQ(l->b[i].descriptorCount, 1);
        CHECK_EQ(l->b[i].stageFlags, e[i].bundle_stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : e[i].bundle_stage == 4 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT);
        CHECK(l->b[i].pImmutableSamplers == NULL);
    }
    mk_obj *pl = mk_obj_of((uint64_t)(uintptr_t)s->layout); CHECK(pl && pl->kind == MK_PL && pl->nset == 1 && pl->set0 == s->set_layout);
}

static void test_shaders(void) {
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, UBD = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    // synthetic bundles (vertex, fragment, compute) - the binding kinds DOS2's packs have (vkn_shader.h)
    xbg_vkn_stage *vs = mk_bundle_add("a000000000000001", 0);
    mk_bind(vs, 0, UB, XBG_SOURCE_SUPPORT, 0xffffffffu, 1392); mk_bind(vs, 1, UB, XBG_SOURCE_UNIFORM, 2, 256); mk_bind(vs, 2, UB, XBG_SOURCE_UNIFORM, 9, 0);
    mk_bind(vs, 3, UB, XBG_SOURCE_C1, 0, 32);
    static const uint8_t c1[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    vs->c1 = c1; vs->c1_bytes = 20; vs->input_count = 3; vs->inputs[0] = 0; vs->inputs[1] = 1; vs->inputs[2] = 4; vs->input_classes[2] = 1;
    xbg_vkn_stage *fs = mk_bundle_add("b000000000000001", 4);
    mk_bind(fs, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(fs, 1, UB, XBG_SOURCE_UNIFORM, 0, 64); mk_bind(fs, 3, CIS, XBG_SOURCE_TEXTURE, 0, 0);
    mk_bind(fs, 2, CIS, XBG_SOURCE_TEXTURE, 3, 0);   // out of binding order in the bundle: the table is sorted
    fs->bindings[3].flags = 0x80000100u;
    xbg_vkn_stage *cs = mk_bundle_add("c000000000000001", 5);
    mk_bind(cs, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, XBG_SOURCE_IMAGE, 1, 0); mk_bind(cs, 1, UB, XBG_SOURCE_UNIFORM, 1, 128);
    mk_bind(cs, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, XBG_SOURCE_STORAGE, 0, 0); mk_bind(cs, 3, UB, XBG_SOURCE_SUPPORT, 0, 1392);
    xbg_vkn_stage *un = mk_bundle_add("e000000000000001", 4); un->module = VK_NULL_HANDLE; mk_bind(un, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392);
    xbg_vkn_stage *gs = mk_bundle_add("g000000000000001", 0); mk_bind(gs, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392);
    const VkShaderModule vs_mod = vs->module, fs_mod = fs->module; void *const vs_xs = vs->shader;
    vkn_shader_set_dead_fn(dead_rec);

    // vertex + fragment, every uniform buffer dynamic (6 <= the device's 8)
    set_dyn_limit(8);
    const uint64_t dsl0 = mk_created[MK_DSL], st0 = used_of(VKN_ARENA_STATIC);
    vkn_stage_src src[VKN_SRCS]; memset(src, 0, sizeof src);
    src[VKN_SRC_VS] = stage(0, "a000000000000001", 0x80000000u | 1u << 2); src[VKN_SRC_FS] = stage(1, "b000000000000001", 0x80000000u | 1u << 5);
    CHECK_EQ(vkn_shader_create(H(10, 1), src), 0);
    memset(g_ctl[0], 0, CTL); memset(g_ctl[1], 0, CTL);   // the control sections were copied
    const vkn_shader *A = vkn_shader_get(H(10, 1));
    CHECK(A != NULL); if (!A) return;
    CHECK_EQ(mk_xbg_calls, 2); CHECK_EQ(mk_xbg_off_main, 2);   // the translator ran on the worker thread
    CHECK(A->handle == H(10, 1)); CHECK(A->id >= 1); CHECK_EQ(A->stages, 3); CHECK_EQ(A->missing, 0); CHECK_EQ(A->unusable, 0); CHECK_EQ(A->dyn_policy, 1);
    CHECK(!strcmp(A->key[VKN_SRC_VS], "a000000000000001")); CHECK(!strcmp(A->key[VKN_SRC_FS], "b000000000000001")); CHECK_EQ(A->key[VKN_SRC_CS][0], 0);
    CHECK(A->module[VKN_SRC_VS] == vs_mod && A->module[VKN_SRC_FS] == fs_mod && A->xs[VKN_SRC_VS] == vs_xs);
    CHECK_EQ(A->ninputs, 3); CHECK(A->inputs[2] == 4 && A->input_classes[2] == 1 && A->input_classes[0] == 0);
    {
        const exp_bind e[8] = {{0, VKN_BK_SUPPORT, 0, 0, 0, 0, UBD}, {4, VKN_BK_SUPPORT, 1, 0, 1, 4, UBD}, {6, VKN_BK_UNIFORM, 0, 2, 2, 0, UBD},
                               {10, VKN_BK_UNIFORM, 1, 0, 3, 4, UBD}, {12, VKN_BK_UNIFORM, 0, 9, 4, 0, UBD}, {16, VKN_BK_TEXTURE, 1, 3, VKN_NO_DYN, 4, CIS},
                               {18, VKN_BK_C1, 0, 0, 5, 0, UBD}, {22, VKN_BK_TEXTURE, 1, 0, VKN_NO_DYN, 4, CIS}};
        check_table(A, e, 8, "A");
        CHECK_EQ(A->ndyn, 6);
        CHECK_EQ(A->bind[5].flags, 0x80000100u); CHECK_EQ(A->bind[2].bytes, 256);
    }
    CHECK_EQ(mk_created[MK_DSL], dsl0 + 1);
    // c1: the bundle's constants in a static block, a multiple of 16, at the uniform offset alignment
    CHECK_EQ(A->c1[VKN_SRC_VS].size, 32); CHECK_EQ(A->c1[VKN_SRC_VS].off % 64, 0); CHECK_EQ(A->c1[VKN_SRC_FS].size, 0);
    if (A->c1[VKN_SRC_VS].cpu) { CHECK(!memcmp(A->c1[VKN_SRC_VS].cpu, c1, 20)); uint8_t z[12] = {0}; CHECK(!memcmp((uint8_t *)A->c1[VKN_SRC_VS].cpu + 20, z, 12)); }
    CHECK_EQ(used_of(VKN_ARENA_STATIC), st0 + 32);
    vkn_shader_stats ss; vkn_shader_stats_get(&ss);
    CHECK_EQ(ss.alive, 1); CHECK_EQ(ss.created, 1); CHECK_EQ(ss.dyn_policy[1], 1); CHECK_EQ(ss.stages, 2); CHECK_EQ(ss.stages_bundle, 2);
    CHECK_EQ(ss.bind_kind[VKN_BK_UNIFORM], 3); CHECK_EQ(ss.bind_kind[VKN_BK_SUPPORT], 2); CHECK_EQ(ss.bind_kind[VKN_BK_TEXTURE], 2); CHECK_EQ(ss.bind_kind[VKN_BK_C1], 1);
    CHECK_EQ(ss.ubo_no_block, 1);   // fragment slot 0: the engine record (mask bit 5 only) has no block with binding index 0
    CHECK_EQ(ss.c1_bytes, 32); CHECK_EQ(ss.layouts, 1);
    CHECK(vkn_shader_get(H(10, 2)) == NULL); CHECK(vkn_shader_get(H(11, 1)) == NULL);
    // the same bundles again: the layouts are shared by content
    src[VKN_SRC_VS] = stage(0, "a000000000000001", 0); src[VKN_SRC_FS] = stage(1, "b000000000000001", 0);
    CHECK_EQ(vkn_shader_create(H(12, 1), src), 0);
    const vkn_shader *A2 = vkn_shader_get(H(12, 1));
    CHECK(A2 && A2->set_layout == A->set_layout && A2->layout == A->layout && A2->id != A->id); CHECK_EQ(mk_created[MK_DSL], dsl0 + 1);
    // dynamic-offset policy 2 (only engine blocks: 3 <= 3 < 6) and 0 (3 > 2)
    set_dyn_limit(3);
    CHECK_EQ(vkn_shader_create(H(13, 1), src), 0);
    const vkn_shader *B = vkn_shader_get(H(13, 1));
    CHECK(B != NULL);
    if (B) {
        CHECK_EQ(B->dyn_policy, 2); CHECK_EQ(B->ndyn, 3); CHECK(B->set_layout != A->set_layout);
        const exp_bind e[8] = {{0, VKN_BK_SUPPORT, 0, 0, VKN_NO_DYN, 0, UB}, {4, VKN_BK_SUPPORT, 1, 0, VKN_NO_DYN, 4, UB}, {6, VKN_BK_UNIFORM, 0, 2, 0, 0, UBD},
                               {10, VKN_BK_UNIFORM, 1, 0, 1, 4, UBD}, {12, VKN_BK_UNIFORM, 0, 9, 2, 0, UBD}, {16, VKN_BK_TEXTURE, 1, 3, VKN_NO_DYN, 4, CIS},
                               {18, VKN_BK_C1, 0, 0, VKN_NO_DYN, 0, UB}, {22, VKN_BK_TEXTURE, 1, 0, VKN_NO_DYN, 4, CIS}};
        check_table(B, e, 8, "B");
    }
    set_dyn_limit(2);
    CHECK_EQ(vkn_shader_create(H(14, 1), src), 0);
    const vkn_shader *C0 = vkn_shader_get(H(14, 1));
    CHECK(C0 && C0->dyn_policy == 0 && C0->ndyn == 0);
    if (C0) { int none = 1; for (uint32_t i = 0; i < C0->nbind; i++) none &= C0->bind[i].dyn == VKN_NO_DYN && C0->bind[i].type != UBD; CHECK(none); }
    set_dyn_limit(8);
    // a compute shader
    memset(src, 0, sizeof src); src[VKN_SRC_CS] = stage(2, "c000000000000001", 0);
    CHECK_EQ(vkn_shader_create(H(15, 1), src), 0);
    const vkn_shader *K = vkn_shader_get(H(15, 1));
    CHECK(K != NULL);
    if (K) {
        CHECK_EQ(K->stages, 1u << VKN_SRC_CS); CHECK_EQ(K->dyn_policy, 1);
        const exp_bind e[4] = {{5, VKN_BK_IMAGE, 5, 1, VKN_NO_DYN, 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {11, VKN_BK_UNIFORM, 5, 1, 0, 5, UBD},
                               {17, VKN_BK_STORAGE, 5, 0, VKN_NO_DYN, 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {23, VKN_BK_SUPPORT, 5, 0, 1, 5, UBD}};
        check_table(K, e, 4, "K");
        CHECK(vkn_shader_module(K, VKN_STAGE_CS, 0, 0, 0) == cs->module); CHECK(vkn_shader_module(K, VKN_STAGE_VS, 0, 0, 0) == VK_NULL_HANDLE);
    }
    // missing stages: no bundle for the key, no key at all; an unusable bundle; a bundle of another stage
    memset(src, 0, sizeof src);
    src[VKN_SRC_VS] = stage(3, "a000000000000001", 0); src[VKN_SRC_FS] = stage(4, "f00000000000dead", 0);
    CHECK_EQ(vkn_shader_create(H(16, 1), src), 0);
    const vkn_shader *M = vkn_shader_get(H(16, 1));
    CHECK(M && M->stages == 3 && M->missing == 1u << VKN_SRC_FS && !M->module[VKN_SRC_FS]);
    CHECK(vkn_shader_module(M, VKN_STAGE_FS, 0, 0, 0) == VK_NULL_HANDLE); CHECK(vkn_shader_module(M, VKN_STAGE_VS, 0, 0, 0) == vs_mod);
    const int xc0 = mk_xbg_calls;
    src[VKN_SRC_FS] = stage(5, "!nokey0000000000", 0);
    CHECK_EQ(vkn_shader_create(H(17, 1), src), 0);
    const vkn_shader *M2 = vkn_shader_get(H(17, 1));
    CHECK(M2 && M2->missing == 1u << VKN_SRC_FS && M2->key[VKN_SRC_FS][0] == 0); CHECK_EQ(mk_xbg_calls, xc0 + 1);   // only the vertex stage asked the translator
    src[VKN_SRC_FS] = stage(6, "e000000000000001", 0);
    CHECK_EQ(vkn_shader_create(H(18, 1), src), 0);
    const vkn_shader *U = vkn_shader_get(H(18, 1));
    CHECK(U && U->unusable == 1u << VKN_SRC_FS && U->missing == 0 && vkn_shader_module(U, VKN_STAGE_FS, 0, 0, 0) == VK_NULL_HANDLE);
    src[VKN_SRC_FS] = stage(7, "a000000000000001", 0);   // a vertex bundle in the fragment slot
    vkn_shader_stats_get(&ss); const uint64_t mism0 = ss.stage_mismatch;
    CHECK_EQ(vkn_shader_create(H(19, 1), src), 0); CHECK(vkn_shader_get(H(19, 1)) != NULL);
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.stage_mismatch, mism0 + 1);
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.stages_missing, 2); CHECK_EQ(ss.stages_unusable, 1);
    // variants through the export, on the worker thread
    const int al0 = mk_alpha_calls, rt0 = mk_retype_calls, off0 = mk_variant_off_main;
    CHECK(vkn_shader_module(A, VKN_STAGE_VS, 0, 0.0f, 0) == vs_mod); CHECK(vkn_shader_module(A, VKN_STAGE_FS, 0, 0.5f, 0) == fs_mod);
    CHECK(vkn_shader_module(A, VKN_STAGE_FS, 8, 0.5f, 0) == fs_mod); CHECK(vkn_shader_module(A, VKN_STAGE_FS, 0, 0.0f, 0x55) == fs_mod);
    CHECK(vkn_shader_module(A, VKN_STAGE_VS, 5, 0.5f, 0) == vs_mod);
    CHECK_EQ(mk_alpha_calls, al0); CHECK_EQ(mk_retype_calls, rt0);
    CHECK(vkn_shader_module(A, VKN_STAGE_VS, 0, 0.0f, 0x0000000200000005ull) == (VkShaderModule)(uintptr_t)(0x7b00000ull + 5));
    CHECK_EQ(mk_retype_calls, rt0 + 1); CHECK_EQ(mk_retype_code, 0x0000000200000005ull); CHECK(mk_variant_xs == vs_xs);
    CHECK(vkn_shader_module(A, VKN_STAGE_FS, 5, 0.25f, 0) == (VkShaderModule)(uintptr_t)(0x7a00000ull + 5));
    CHECK_EQ(mk_alpha_calls, al0 + 1); CHECK_EQ(mk_alpha_func, 5); CHECK(mk_alpha_ref == 0.25f); CHECK(mk_variant_xs == A->xs[VKN_SRC_FS]);
    CHECK_EQ(mk_variant_off_main, off0 + 2);
    CHECK(vkn_shader_module(A, VKN_STAGE_CS, 0, 0, 0) == VK_NULL_HANDLE); CHECK(vkn_shader_module(A, 3, 0, 0, 0) == VK_NULL_HANDLE); CHECK(vkn_shader_module(NULL, 0, 0, 0, 0) == VK_NULL_HANDLE);
    // destroy: the dead listener (here wired to the pipeline cache, as the host must), the record gone, its c1 block and id back after the delay
    vkn_pipe_desc pd; memset(&pd, 0, sizeof pd); pd.shader = A->id; pd.prim = 1; pd.pass = g_c1; pd.write_mask[0] = 0xf;
    const vkn_pipe_key pk = vkn_pipe_key_pack(&pd);
    const vkn_pipe_src ps = {A->module[VKN_SRC_VS], A->module[VKN_SRC_FS], A->layout, VK_NULL_HANDLE, 0};
    const VkPipeline pa = vkn_pipe_get(&pk, &ps); CHECK(pa != VK_NULL_HANDLE);
    const uint32_t a_id = A->id; const uint64_t st1 = used_of(VKN_ARENA_STATIC);
    g_ndead = 0;
    vkn_shader_destroy(H(10, 1));
    CHECK_EQ(g_ndead, 1); CHECK_EQ(g_dead_id[0], a_id); CHECK_EQ(g_dead_h[0], H(10, 1));
    CHECK(vkn_shader_get(H(10, 1)) == NULL);
    vkn_shader_destroy(H(10, 1)); CHECK_EQ(g_ndead, 1);
    frames(2); CHECK_EQ(used_of(VKN_ARENA_STATIC), st1); CHECK(mk_is_live((uint64_t)(uintptr_t)pa, MK_PIPE));
    frames(1); CHECK_EQ(used_of(VKN_ARENA_STATIC), st1 - 32); CHECK(!mk_is_live((uint64_t)(uintptr_t)pa, MK_PIPE));
    CHECK(mk_is_live((uint64_t)(uintptr_t)A2->set_layout, MK_DSL));   // shared layouts stay
    memset(src, 0, sizeof src); src[VKN_SRC_VS] = stage(8, "g000000000000001", 0);
    CHECK_EQ(vkn_shader_create(H(20, 1), src), 0);
    const vkn_shader *R = vkn_shader_get(H(20, 1)); CHECK(R && R->id == a_id);   // the id is reused once the record is retired
    // a create over a live index (another generation): the old record is replaced and reported dead
    g_ndead = 0; const uint32_t b_id = B ? B->id : 0;
    src[VKN_SRC_VS] = stage(9, "a000000000000001", 0);
    CHECK_EQ(vkn_shader_create(H(13, 2), src), 0);
    const vkn_shader *B2 = vkn_shader_get(H(13, 2)); CHECK(B2 != NULL); CHECK(vkn_shader_get(H(13, 1)) == NULL);
    CHECK_EQ(g_ndead, 1); CHECK_EQ(g_dead_id[0], b_id);
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.replaced, 1);
    // a destroy of a creation still queued cancels it: the worker is held inside the translator by the gate
    pthread_mutex_lock(&mk_gate_mu); mk_gate_on = 1; memcpy(mk_gate_key, "g000000000000001", 17); pthread_mutex_unlock(&mk_gate_mu);
    src[VKN_SRC_VS] = stage(10, "g000000000000001", 0);
    CHECK_EQ(vkn_shader_create(H(21, 1), src), 0);
    pthread_mutex_lock(&mk_gate_mu); while (!mk_gate_inside) pthread_cond_wait(&mk_gate_cv, &mk_gate_mu); pthread_mutex_unlock(&mk_gate_mu);
    src[VKN_SRC_VS] = stage(11, "a000000000000001", 0);
    CHECK_EQ(vkn_shader_create(H(22, 1), src), 0);
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.queued, 1); const uint64_t can0 = ss.cancelled, cr0 = ss.created;
    vkn_shader_destroy(H(22, 1));
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.cancelled, can0 + 1); CHECK_EQ(ss.queued, 0);
    pthread_mutex_lock(&mk_gate_mu); mk_gate_on = 0; pthread_cond_broadcast(&mk_gate_cv); pthread_mutex_unlock(&mk_gate_mu);
    CHECK(vkn_shader_get(H(21, 1)) != NULL); CHECK(vkn_shader_get(H(22, 1)) == NULL);
    vkn_shader_sync();
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.created, cr0 + 1);
    // refusals: no handle, no sources, an index beyond the table (built, not kept), the translator not up
    CHECK_EQ(vkn_shader_create(0, src), -1); CHECK_EQ(vkn_shader_create(H(23, 1), NULL), -1);
    vkn_shader_stats_get(&ss); const uint64_t fail0 = ss.failed;
    CHECK_EQ(vkn_shader_create(H(1u << 16, 1), src), 0); CHECK(vkn_shader_get(H(1u << 16, 1)) == NULL); vkn_shader_sync();
    mk_xbg_down = 1; CHECK_EQ(vkn_shader_create(H(24, 1), src), 0); CHECK(vkn_shader_get(H(24, 1)) == NULL); mk_xbg_down = 0;
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.failed, fail0 + 2);
    // the SUPPORT block per viewport size (xbg_support_defaults' layout), cached
    vkn_block sb, sb2, sb3;
    CHECK_EQ(vkn_shader_support(1280.0f, 720.0f, &sb), 0);
    CHECK(sb.size >= 1392); CHECK_EQ(sb.off % 64, 0);
    const float *f = (const float *)sb.cpu;
    CHECK(f[144 / 4] == 1.0f / 1280.0f && f[148 / 4] == 1.0f / 720.0f && f[160 / 4] == 1280.0f && f[164 / 4] == 720.0f && f[192 / 4] == 1.0f && f[192 / 4 + 4 * 72] == 1.0f && f[192 / 4 + 1] == 0.0f && f[192 / 4 + 4] == 1.0f && f[0] == 0.0f);   // (std140 stride 16)
    CHECK_EQ(vkn_shader_support(1280.0f, 720.0f, &sb2), 0); CHECK(sb2.buf == sb.buf && sb2.off == sb.off);
    CHECK_EQ(vkn_shader_support(640.0f, 360.0f, &sb3), 0); CHECK(sb3.off != sb.off); CHECK(((const float *)sb3.cpu)[160 / 4] == 640.0f);
    CHECK_EQ(vkn_shader_support(0.0f, 720.0f, &sb3), -1); CHECK_EQ(vkn_shader_support(1.0f, 1.0f, NULL), -1);
    vkn_shader_census();
    // everything destroyed: no record alive, the c1 blocks back
    static const uint32_t live[] = {12, 14, 15, 16, 17, 18, 19, 20, 21};
    for (unsigned i = 0; i < sizeof live / sizeof *live; i++) vkn_shader_destroy(H(live[i], 1));
    vkn_shader_destroy(H(13, 2));
    frames(3);
    vkn_shader_stats_get(&ss); CHECK_EQ(ss.alive, 0); CHECK_EQ(ss.c1_bytes, 0); CHECK_EQ(ss.table_bytes, 0); CHECK_EQ(ss.layouts_own, 0);
    CHECK_EQ(ss.dyn_policy[0] + ss.dyn_policy[1] + ss.dyn_policy[2], 0);
    CHECK_EQ(used_of(VKN_ARENA_STATIC), st0 + 2 * 1392);   // only the two SUPPORT blocks (kept for the process)
    CHECK_EQ(mk_live(MK_PIPE), 0);
}

// [DRS 21.11] dynamic resolution changes the 3D size mid-run (phone run S08, 1485x668 = 68.7 % .. 100 % of 2160x972): every step brings
// new viewport sizes (the 3D part and its half / quarter / eighth chain). The SUPPORT cache held 32 sizes for the process: after a few
// steps vkn_shader_support failed for every new size and the draw path bound the zero block (1/w, 1/h = 0 -> each full-screen pass
// sampled one texel: a flat screen under the HUD, the UI dropped its draws). Now: no failure over hundreds of sizes, every block holds
// its size, a size looked up every frame is never rewritten, and when every size was used within the last 16 frames the caller gets
// -1 and the bytes for a frame-local copy (vkn_shader_support_fill).
static int sup_is(const vkn_block *b, float w, float h) {
    const float *f = (const float *)b->cpu;
    return f[144 / 4] == 1.0f / w && f[148 / 4] == 1.0f / h && f[160 / 4] == w && f[164 / 4] == h && f[192 / 4] == 1.0f && f[192 / 4 + 4 * 72] == 1.0f && f[192 / 4 + 1] == 0.0f;
}
static void test_support_drs(void) {
    vkn_block win, b;
    CHECK_EQ(vkn_shader_support(2400.0f, 1080.0f, &win), 0);   // the window: the UI and the present pass, every frame
    uint64_t rec0 = 0, full0 = 0, fills0 = 0; vkn_shader_support_stats(NULL, &rec0, &full0, &fills0);
    int fails = 0, wrong = 0, win_moved = 0;
    for (int step = 0; step < 200; step++) {   // 200 DRS steps of 2 frames: 800 sizes of the 3D chain
        const float w = 1485.0f + 3.0f * (float)step, h = 668.0f + 1.5f * (float)step;
        for (int fr = 0; fr < 2; fr++) {
            vkn_block wb;
            if (vkn_shader_support(2400.0f, 1080.0f, &wb)) fails++;
            else if (wb.off != win.off || wb.buf != win.buf || !sup_is(&wb, 2400.0f, 1080.0f)) win_moved++;
            for (int k = 0; k < 4; k++) {
                const float cw = w / (float)(1 << k), ch = h / (float)(1 << k);
                if (vkn_shader_support(cw, ch, &b)) fails++;
                else if (!sup_is(&b, cw, ch)) wrong++;
            }
            frames(1);
        }
    }
    CHECK_EQ(fails, 0); CHECK_EQ(wrong, 0); CHECK_EQ(win_moved, 0);
    uint32_t n = 0; uint64_t rec = 0, full = 0, fills = 0; vkn_shader_support_stats(&n, &rec, &full, &fills);
    CHECK_EQ(n, 256); CHECK(rec > rec0 + 400); CHECK_EQ(full, full0);
    // back to a size of an early step (rewritten long ago): a fresh block with that size's bytes
    CHECK_EQ(vkn_shader_support(1485.0f, 668.0f, &b), 0); CHECK(sup_is(&b, 1485.0f, 668.0f));
    // a frame that uses more sizes than the cache can give without touching a block the GPU may still read: -1, then the copy's bytes
    int got = 0, refused = 0;
    for (int i = 0; i < 300; i++) { if (vkn_shader_support(100.0f + (float)i, 50.0f, &b)) refused++; else got++; }
    CHECK(refused > 0); CHECK(got > 0); CHECK(got <= 256);
    vkn_shader_support_stats(NULL, NULL, &full, NULL); CHECK_EQ(full, full0 + (uint64_t)refused);
    uint8_t copy[1392]; vkn_block cb = {0}; cb.cpu = copy;
    CHECK_EQ(vkn_shader_support_fill(399.0f, 50.0f, copy, sizeof copy), 0); CHECK(sup_is(&cb, 399.0f, 50.0f));
    CHECK_EQ(vkn_shader_support_fill(399.0f, 50.0f, copy, 16), -1); CHECK_EQ(vkn_shader_support_fill(0.0f, 50.0f, copy, sizeof copy), -1);
    vkn_shader_support_stats(NULL, NULL, NULL, &fills); CHECK_EQ(fills, fills0 + 1);
    frames(20);   // idle again: the sizes come back
    CHECK_EQ(vkn_shader_support(2160.0f, 972.0f, &b), 0); CHECK(sup_is(&b, 2160.0f, 972.0f));
    vkn_shader_census();
}

static void test_leaks(void) {
    vkn_retire_all();
    CHECK_EQ(mk_live(MK_IMAGE), 0); CHECK_EQ(mk_live(MK_VIEW), 0); CHECK_EQ(mk_live(MK_SAMPLER), 0); CHECK_EQ(mk_live(MK_FB), 0); CHECK_EQ(mk_live(MK_PIPE), 0);
    vkn_shader_stats ss; vkn_shader_stats_get(&ss); vkn_state_stats st; vkn_state_stats_get(&st);
    CHECK_EQ(mk_live(MK_DSL), ss.layouts + 1); CHECK_EQ(mk_live(MK_PL), ss.layouts + 1);   // the shared layout cache + the pipeline test's own
    CHECK_EQ(mk_live(MK_RP), st.passes);                                                   // passes live for the process
    CHECK_EQ(mk_live(MK_MEMORY), mk_live(MK_BUFFER));                                      // only arena chunks are left: one memory per chunk
    CHECK_EQ(used_of(VKN_ARENA_STAGING), 0); CHECK_EQ(vkn_upload_pending_bytes(), 0);
    uint64_t u[VKN_ARENAS], r[VKN_ARENAS]; vkn_arena_stats(u, r);
    CHECK_EQ(u[VKN_ARENA_RING], VKN_RING_BYTES);
    CHECK_EQ(mk_qlock_depth, 0);
}
