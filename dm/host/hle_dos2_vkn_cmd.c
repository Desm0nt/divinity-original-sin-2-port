// [DOS2] VK-NATIVE stage 3, part "NVN command entry points" (dm_vkn_cmd_register; DM_VK_NATIVE=1 only): what is left of the game's
// direct NVN calls in a native frame goes to this file instead of HLE NVN. main = main.raw of update v786432, build 4979b200; r = the
// renderer ([main+0x2bbdc68]); the game's NVN function table = 515 u64 slots at main+0x2bbee98 (slot i at +8i; index -> name:
// src/work/re-nr-nrend/nvn_slot_map.tsv), filled by 0x934d70(device, GetProcAddress) twice in device init 0x938330 (0x93852c with a
// null device, 0x938560 after nvnDeviceInitialize); every engine call is `ldr xN, [table + 8i]; blr xN`.
//
// Design (docs/dos2/nr/VK-NATIVE-STATUS.md, stage 3 wave 2): the other parts replace the renderer's functions and the packet handlers
// with NVN inside, but the engine still calls NVN directly from (a) the immediate branches of the two-mode stage functions (a command
// list with u8 +0x30 == 0 executes at once: 0xa7eac0 builds one), (b) the frame function 0xa66bc0 (screenshot / thumbnail branch),
// (c) the UI block 0x1bea840..0x1bf4338 (the UI part replaces its entries), (d) creation-time code (builders, pools, Initialize:
// these slots stay HLE NVN until stage 4). When device init 0x938330 returns (both runs of 0x934d70 are inside it, and so is its
// tail 0x939b00 -> 0x939850 that creates the command buffers: those calls still reach HLE NVN directly) this file repoints the slots
// of every nvnCommandBuffer*
// recording entry and of the queue / sync entries that only make sense for an NVN command stream (CMD_SLOTS below) to its own host
// functions, so HLE NVN command recording is never entered in mode 1. Not repointed (creation-time / queries, HLE NVN keeps them):
// CommandBufferInitialize / Finalize / SetDebugLabel / SetMemoryCallback(Data) / SetCommandMemoryCallbackEnabled / AddCommandMemory /
// AddControlMemory (callers 0x939850 context create and the replaced BeginFrame 0x9215f0), the Get* / IsRecording queries, and all
// non-command entries (Device*, Queue builders / Initialize, Window builders / Initialize / SetCrop, Sync / Event objects).
// Each repointed slot is one of:
//   ACTION   the engine-level meaning performed through the backend: DrawElements / DrawElementsInstanced / DrawArrays (the renderer
//            cache read by the draw part's dm_vkn_draw_fill; first index from the NVN address: an index buffer of mode 1 carries the
//            pseudo GPU address vkn_buf_va of its block in its record +8 (hle_dos2_vkn_buf.c), the bound one is r+0x17c8), SetViewport /
//            SetScissor (no engine field written - the callers write the caches themselves, see the verdicts; the backend's "in effect"
//            rectangles: dm_vkn_exec_viewport_note, else an override, dm_vkn_cmd_override below), ClearTexture, ClearDepthStencil,
//            SetRenderTargets (NVN texture objects -> vkn_tex_by_cookie).
//   COUNTED  a no-op with NVN's benign result: EndRecording -> handle 0, SyncWait -> CONDITION_SATISFIED, QueueWaitSync -> TRUE,
//            everything else returns void.
//   FORWARD  the HLE NVN function the game's table held: WindowAcquireTexture (only the replaced BeginFrame calls it: a call means a
//            missing hook, the HLE keeps its pacing), and ANY slot called from outside main's text (host code) or from the
//            creation-time functions 0x938330 / 0x939850 (QueueFenceSync of a new context's sync at 0x9399dc; they run inside device
//            init, before the repointing, so this is a safety net).
// Every call is counted by slot and by the caller's return address (main-relative): census every 10 s `stray NVN calls: N ...` with
// the top sites; a site that is not in k_known (the verdict table below, 193 call sites generated from the disassembly) prints a
// red line once and is listed in red in every census window it is called in.
//
// ---- Verdicts of the call sites outside the replaced functions and the UI block (decompilations + disassembly of each function;
// "cache" = the function writes the renderer cache field with the compare-with-cache rule of the packet / list wrapper it inlines,
// THEN calls NVN: the next native draw reads the same state from the cache, the NVN call is redundant) ----
//   state binds, all COUNTED (cache-consistent):
//     0xabda50  BindPolygonState (r+0x16f0), BindDepthStencilState + SetStencilMask / ValueMask / Ref(0) (r+0x1708, r+0x1710 = 0),
//               BindVertexAttribState + BindVertexStreamState (r+0x1740), BindVertexBuffer(0) (r+0x1748 / r+0x1788 = 0, record +0x38
//               stamped by the engine), index buffer and primitive cache-only (r+0x17c8 / r+0x17d0, r+0x1714 / r+0x1718)
//     0xb23970  depth-stencil + stencil (r+0x1708 / 0x1710), vertex format (r+0x1740), VB0..2 (r+0x1748..0x1758, offsets 0),
//               SetRenderEnable(Conditional) (r+0x185c / r+0x1860 written, r+0x185d = 0) - the packet 0x15 shape inlined
//     0x142c870 the same set as 0xb23970 (depth-stencil, VB0..2, vertex format, render enable)
//     0xb0aab0  depth-stencil [main+0x2bbdb88] (r+0x1708, ref 0), polygon [main+0x2bbdc20] (r+0x16f0), primitive 1 (r+0x1714 / 0x1718)
//     0xd1ada0  depth-stencil [main+0x2bbdba0], polygon [main+0x2bbdc20], vertex format (r+0x1740), VB0 (r+0x1748), primitive 1
//     0xafa7c0  BindPolygonState x2 (r+0x16f0: the inlined SetRasterState 0x93ffc0)
//     SetRenderEnable before every immediate draw below: the engine itself clears r+0x185c / r+0x185d / r+0x1860 (the draw tail)
//   draws, ACTION (the engine ran the tail before: 0x926f90 / 0x925740 / 0x924730 are the draw part's hooks, the render-enable reset
//   and r+0x185d = r+0x185c after the call are engine code; so the action is dm_vkn_draw_fill + vkn_draw_* only, no tail):
//     DrawElements(cb, r+0x1718, IB record +0x24, count, IB record +8 [+ first * size]): 0xaab150 0x1240ee0 0x1335a30 0xabda50
//       0xabe690 0xabebf0 0xabf1e0 0xaf2ed0 0xab9fa0 (x2) 0xb89330 (x2) - every one with the IB of r+0x17c8
//     DrawArrays(cb, r+0x1718, 0, 3): 0xb0aab0 0xd1ada0 (full-screen triangles)
//   viewport / scissor, ACTION: every SetViewport(x, y, w, h) outside the replaced functions is preceded by the write of the viewport
//     cache r+0x171c {w, h, x, y} (unless equal) and followed by SetScissor(x, y, w, h) of the same rectangle WITHOUT writing the
//     scissor cache r+0x172c (the packet 0x12 shape inlined): 0xab50e0 0xab5390 0xb0a880 0xb15090 0xb20220 0xb20650 0xb0aab0 0xd1ada0
//     0xd1fd70 -> dm_vkn_exec_viewport_note (the exec part's model then says "scissor = viewport rectangle", which is what NVN has).
//     0xd1fd70's second SetScissor (0xd201e8) writes r+0x172c {l, t, r, b} first (the packet 0x13 shape) - the exec part never sees
//     it: an override rectangle (dm_vkn_cmd_override) valid until the caches change or a new recording begins.
//   render targets, ACTION: 0xd1fd70 (0xd20104 / 0xd20190): the Iggy UI object [main+0x32c6a70] rebinds its own colour target
//     (+0xb0 or [[+0x1b0]]) and depth (+0xb8) after the engine's 0x9279a0 bound param+0xd0 - behind the engine cache r+0x17e8 / r+0x1828
//     (the next 0x9279a0 replacement calls vkn_pass_targets unconditionally, so the backend follows NVN either way)
//   clears, ACTION: 0x14227a0 (0x1422b74 / 0x1422c98) ClearTexture(texture object, view levels 0..1 layers 0..1, region, colour
//     main+0x1f5a270, mask 0xf) - packet 0x04 inlined for the immediate list
//   barriers, COUNTED (the pass part orders and transitions per use; NVN's Barrier(cb, 2) has nothing left to do): 79 sites in
//     0xa66bc0 0xab30e0 0xab46e0 0xab5a00 0xab6e90 0xab71f0 0xab9fa0 0xac1270 0xac1a80 0xac2de0 0xac3260 0xac6070 0xac8d10 0xb0aab0
//     0xb1c580 0xb1d6a0 0xd20d40 0x14220b0 0x14227a0 0x1424560
//   counters, COUNTED (ReportCounter: queries are stage 4, as the exec part's packets 0x3d / 0x3e): 0x90cbf0 0xb7e020 0xb87470 0xb89330
//   frame, COUNTED: 0xa66bc0's read-back branch Barrier, EndRecording + QueueSubmitCommands (for a window other than dev[10]), then
//     QueueFenceSync + QueueFlush + SyncWait(ctx+0xd8, ~0): the window's EndFrame(win, 0) between them (frame part: vkn_rec_submit(1))
//     does the real submit and wait, so EndRecording returns handle 0, the submit is dropped and SyncWait answers "satisfied".
//   creation, FORWARD: 0x939850 (context create: QueueFenceSync of ctx+0xd8; Initialize / Add*Memory / SetMemoryCallback are not
//     repointed at all) - reached only if a context were created after device init.
// Not handled here (another part's): 0xd1ada0 also writes video planes straight into NVN texture storage (TextureGetStride +
// memcpy into [record + 0xe0], three textures: the packet 0x2c shape) - the backend's images of those textures do not see it.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_buf.h"
#include "../gfx/vkn/vkn_draw.h"
#include "../gfx/vkn/vkn_frame.h"
#include "../gfx/vkn/vkn_pass.h"
#include "../gfx/vkn/vkn_tex.h"
#include "../gfx/nvn/nvn_obj.h"   // view_t: the HLE layout of NVNtextureView in guest memory (levels / layers of a view argument)
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_draw.h"
#include "hle_dos2_vkn_exec.h"
#include "hle_dos2_vkn_stack.h"

#define CMD_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own

module_t *dn_main(void);   // hle_dos2_native.c

// ---- what other parts may call (weak declarations on their side; mode 1 only) ----
// int dm_vkn_cmd_override(uint64_t r, float vp[4], int32_t sc[4]): the viewport / scissor a DIRECT NVN call set behind the renderer's
//     caches (bit 0: vp = {x, y, w, h} replaces vkn_draw_in.viewport, bit 1: sc = {x, y, w, h} replaces vkn_draw_in.scissor; 0 = the
//     caches and dm_vkn_exec_scissor are right). This file's draws apply it; the draw part's dm_vkn_draw_fill should call it last
//     (one weak call) so list draws after such a call see it too. Known producer: 0xd1fd70's UI scissor (0xd201e8).
// uint64_t dm_vkn_cmd_calls(void): direct NVN calls that reached this file so far (all slots).
int dm_vkn_cmd_override(uint64_t r, float vp[4], int32_t sc[4]);
uint64_t dm_vkn_cmd_calls(void);
void dm_vkn_cmd_register(void);

// ---- engine facts ----
enum {
    M_TABLE = 0x2bbee98, TABLE_SLOTS = 515,            // the game's NVN function table
    M_PRIM_TAB = 0x1f58640,                            // 7 x u32: engine primitive 1..7 -> NVN draw mode (0x93e5f0 opcode 0x0d)
    R_PRIM = 0x1714, R_NVN_PRIM = 0x1718, R_VP = 0x171c /* {w, h, x, y} */, R_SC = 0x172c /* {l, t, r, b} */, R_IB = 0x17c8,
    F_CREATE_DEV = 0x938330, F_CREATE_DEV_END = 0x9390b0,   // device init (size 0xd80)
    F_CREATE_CTX = 0x939850, F_CREATE_CTX_END = 0x939b00,   // context create (size 0x2b0)
    OFF_HOST = 0xffffffffu,                            // "return address outside main's text"
    NVN_SYNC_SATISFIED = 1,                            // NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED
};

// ---- the repointed slots: X(index, name without "nvn", kind) ----
enum { K_NOP, K_ACT, K_RET, K_CREATE, K_FWD };
#define CMD_SLOTS(X) \
    X(57, "QueueSubmitCommands", K_NOP) X(58, "QueueFlush", K_NOP) X(59, "QueueFinish", K_NOP) X(60, "QueuePresentTexture", K_NOP) \
    X(77, "WindowAcquireTexture", K_FWD) X(366, "CommandBufferBeginRecording", K_NOP) X(367, "CommandBufferEndRecording", K_RET) \
    X(368, "CommandBufferCallCommands", K_NOP) X(369, "CommandBufferCopyCommands", K_NOP) \
    X(370, "CommandBufferBindBlendState", K_NOP) X(371, "CommandBufferBindChannelMaskState", K_NOP) \
    X(372, "CommandBufferBindColorState", K_NOP) X(373, "CommandBufferBindMultisampleState", K_NOP) \
    X(374, "CommandBufferBindPolygonState", K_NOP) X(375, "CommandBufferBindDepthStencilState", K_NOP) \
    X(376, "CommandBufferBindVertexAttribState", K_NOP) X(377, "CommandBufferBindVertexStreamState", K_NOP) \
    X(378, "CommandBufferBindProgram", K_NOP) X(379, "CommandBufferBindVertexBuffer", K_NOP) \
    X(380, "CommandBufferBindVertexBuffers", K_NOP) X(381, "CommandBufferBindUniformBuffer", K_NOP) \
    X(382, "CommandBufferBindUniformBuffers", K_NOP) X(383, "CommandBufferBindTransformFeedbackBuffer", K_NOP) \
    X(384, "CommandBufferBindTransformFeedbackBuffers", K_NOP) X(385, "CommandBufferBindStorageBuffer", K_NOP) \
    X(386, "CommandBufferBindStorageBuffers", K_NOP) X(387, "CommandBufferBindTexture", K_NOP) \
    X(388, "CommandBufferBindTextures", K_NOP) X(389, "CommandBufferBindImage", K_NOP) X(390, "CommandBufferBindImages", K_NOP) \
    X(391, "CommandBufferSetPatchSize", K_NOP) X(392, "CommandBufferSetInnerTessellationLevels", K_NOP) \
    X(393, "CommandBufferSetOuterTessellationLevels", K_NOP) X(394, "CommandBufferSetPrimitiveRestart", K_NOP) \
    X(395, "CommandBufferBeginTransformFeedback", K_NOP) X(396, "CommandBufferEndTransformFeedback", K_NOP) \
    X(397, "CommandBufferPauseTransformFeedback", K_NOP) X(398, "CommandBufferResumeTransformFeedback", K_NOP) \
    X(399, "CommandBufferDrawTransformFeedback", K_NOP) X(400, "CommandBufferDrawArrays", K_ACT) \
    X(401, "CommandBufferDrawElements", K_ACT) X(402, "CommandBufferDrawElementsBaseVertex", K_NOP) \
    X(403, "CommandBufferDrawArraysInstanced", K_NOP) X(404, "CommandBufferDrawElementsInstanced", K_ACT) \
    X(405, "CommandBufferDrawArraysIndirect", K_NOP) X(406, "CommandBufferDrawElementsIndirect", K_NOP) \
    X(407, "CommandBufferMultiDrawArraysIndirectCount", K_NOP) X(408, "CommandBufferMultiDrawElementsIndirectCount", K_NOP) \
    X(409, "CommandBufferClearColor", K_NOP) X(410, "CommandBufferClearColori", K_NOP) X(411, "CommandBufferClearColorui", K_NOP) \
    X(412, "CommandBufferClearDepthStencil", K_ACT) X(413, "CommandBufferDispatchCompute", K_NOP) \
    X(414, "CommandBufferDispatchComputeIndirect", K_NOP) X(415, "CommandBufferSetViewport", K_ACT) \
    X(416, "CommandBufferSetViewports", K_NOP) X(417, "CommandBufferSetViewportSwizzles", K_NOP) \
    X(418, "CommandBufferSetScissor", K_ACT) X(419, "CommandBufferSetScissors", K_NOP) X(420, "CommandBufferSetDepthRange", K_NOP) \
    X(421, "CommandBufferSetDepthBounds", K_NOP) X(422, "CommandBufferSetDepthRanges", K_NOP) \
    X(423, "CommandBufferSetTiledCacheAction", K_NOP) X(424, "CommandBufferSetTiledCacheTileSize", K_NOP) \
    X(425, "CommandBufferBindSeparateTexture", K_NOP) X(426, "CommandBufferBindSeparateSampler", K_NOP) \
    X(427, "CommandBufferBindSeparateTextures", K_NOP) X(428, "CommandBufferBindSeparateSamplers", K_NOP) \
    X(429, "CommandBufferSetStencilValueMask", K_NOP) X(430, "CommandBufferSetStencilMask", K_NOP) \
    X(431, "CommandBufferSetStencilRef", K_NOP) X(432, "CommandBufferSetBlendColor", K_NOP) \
    X(433, "CommandBufferSetPointSize", K_NOP) X(434, "CommandBufferSetLineWidth", K_NOP) \
    X(435, "CommandBufferSetPolygonOffsetClamp", K_NOP) X(436, "CommandBufferSetAlphaRef", K_NOP) \
    X(437, "CommandBufferSetSampleMask", K_NOP) X(438, "CommandBufferSetRasterizerDiscard", K_NOP) \
    X(439, "CommandBufferSetDepthClamp", K_NOP) X(440, "CommandBufferSetConservativeRasterEnable", K_NOP) \
    X(441, "CommandBufferSetConservativeRasterDilate", K_NOP) X(442, "CommandBufferSetSubpixelPrecisionBias", K_NOP) \
    X(443, "CommandBufferCopyBufferToTexture", K_NOP) X(444, "CommandBufferCopyTextureToBuffer", K_NOP) \
    X(445, "CommandBufferCopyTextureToTexture", K_NOP) X(446, "CommandBufferCopyBufferToBuffer", K_NOP) \
    X(447, "CommandBufferClearBuffer", K_NOP) X(448, "CommandBufferClearTexture", K_ACT) \
    X(449, "CommandBufferClearTexturei", K_NOP) X(450, "CommandBufferClearTextureui", K_NOP) \
    X(451, "CommandBufferUpdateUniformBuffer", K_NOP) X(452, "CommandBufferReportCounter", K_NOP) \
    X(453, "CommandBufferResetCounter", K_NOP) X(454, "CommandBufferReportValue", K_NOP) \
    X(455, "CommandBufferSetRenderEnable", K_NOP) X(456, "CommandBufferSetRenderEnableConditional", K_NOP) \
    X(457, "CommandBufferSetRenderTargets", K_ACT) X(458, "CommandBufferDiscardColor", K_NOP) \
    X(459, "CommandBufferDiscardDepthStencil", K_NOP) X(460, "CommandBufferDownsample", K_NOP) \
    X(461, "CommandBufferTiledDownsample", K_NOP) X(462, "CommandBufferDownsampleTextureView", K_NOP) \
    X(463, "CommandBufferTiledDownsampleTextureView", K_NOP) X(464, "CommandBufferBarrier", K_NOP) \
    X(465, "CommandBufferWaitSync", K_NOP) X(466, "CommandBufferFenceSync", K_NOP) X(467, "CommandBufferSetTexturePool", K_NOP) \
    X(468, "CommandBufferSetSamplerPool", K_NOP) X(469, "CommandBufferSetShaderScratchMemory", K_NOP) \
    X(470, "CommandBufferSaveZCullData", K_NOP) X(471, "CommandBufferRestoreZCullData", K_NOP) \
    X(472, "CommandBufferSetCopyRowStride", K_NOP) X(473, "CommandBufferSetCopyImageStride", K_NOP) \
    X(476, "CommandBufferDrawTexture", K_NOP) X(478, "CommandBufferSetProgramSubroutines", K_NOP) \
    X(479, "CommandBufferBindCoverageModulationTable", K_NOP) X(480, "CommandBufferResolveDepthBuffer", K_NOP) \
    X(481, "CommandBufferSetColorReductionEnable", K_NOP) X(482, "CommandBufferSetColorReductionThresholds", K_NOP) \
    X(483, "CommandBufferPushDebugGroupStatic", K_NOP) X(484, "CommandBufferPushDebugGroupDynamic", K_NOP) \
    X(485, "CommandBufferPushDebugGroup", K_NOP) X(486, "CommandBufferPopDebugGroup", K_NOP) \
    X(487, "CommandBufferPopDebugGroupId", K_NOP) X(488, "CommandBufferInsertDebugMarkerStatic", K_NOP) \
    X(489, "CommandBufferInsertDebugMarkerDynamic", K_NOP) X(490, "CommandBufferInsertDebugMarker", K_NOP) \
    X(497, "QueueFenceSync", K_CREATE) X(498, "SyncWait", K_RET) X(499, "QueueWaitSync", K_RET) \
    X(513, "CommandBufferWaitEvent", K_NOP) X(514, "CommandBufferSignalEvent", K_NOP)
enum {   // the slots this file acts on by index
    S_SUBMIT = 57, S_ACQUIRE = 77, S_END_REC = 367, S_DRAW_ARRAYS = 400, S_DRAW_ELEMENTS = 401, S_DRAW_ELEMENTS_INST = 404,
    S_CLEAR_DS = 412, S_VIEWPORT = 415, S_SCISSOR = 418, S_CLEAR_TEX = 448, S_RT = 457, S_FENCE = 497, S_SYNC_WAIT = 498,
    S_QUEUE_WAIT_SYNC = 499,
};

// ---- the verdict table: every direct call site outside the replaced functions and the UI block (return address main-relative,
// slot, verdict), generated from the disassembly (blr through a register loaded from the table), sorted by address ----
enum { V_REDUNDANT, V_BARRIER, V_QUERY, V_DRAW, V_VPSC, V_RT, V_CLEAR, V_FRAME, V_CREATE, V_N };
static const char *const k_vname[V_N] = {"cache-consistent state", "barrier", "counter", "draw", "viewport/scissor", "targets", "clear", "frame sink", "creation"};
typedef struct { uint32_t ra; uint16_t slot; uint8_t v; } cmd_known;
static const cmd_known k_known[] = {
    {0x90cffc, 452, V_QUERY}, {0x90d028, 452, V_QUERY}, {0x9398cc, 352, V_CREATE}, {0x939978, 358, V_CREATE},
    {0x939990, 359, V_CREATE}, {0x9399a8, 355, V_CREATE}, {0x9399e0, 497, V_CREATE}, {0xa677f0, 464, V_FRAME},
    {0xa67998, 367, V_FRAME}, {0xa679bc, 57, V_FRAME}, {0xa67a30, 497, V_FRAME}, {0xa67a48, 58, V_FRAME}, {0xa67a5c, 498, V_FRAME},
    {0xaab250, 401, V_DRAW}, {0xaab2a4, 455, V_REDUNDANT}, {0xab3f60, 464, V_BARRIER}, {0xab3f84, 464, V_BARRIER},
    {0xab3fa8, 464, V_BARRIER}, {0xab3fcc, 464, V_BARRIER}, {0xab3ff0, 464, V_BARRIER}, {0xab4014, 464, V_BARRIER},
    {0xab4038, 464, V_BARRIER}, {0xab405c, 464, V_BARRIER}, {0xab4080, 464, V_BARRIER}, {0xab40a4, 464, V_BARRIER},
    {0xab4ff8, 464, V_BARRIER}, {0xab501c, 464, V_BARRIER}, {0xab5040, 464, V_BARRIER}, {0xab5064, 464, V_BARRIER},
    {0xab5088, 464, V_BARRIER}, {0xab50ac, 464, V_BARRIER}, {0xab5250, 415, V_VPSC}, {0xab5270, 418, V_VPSC}, {0xab548c, 415, V_VPSC},
    {0xab54ac, 418, V_VPSC}, {0xab60a0, 464, V_BARRIER}, {0xab70a4, 464, V_BARRIER}, {0xab739c, 464, V_BARRIER},
    {0xabb1d8, 464, V_BARRIER}, {0xabcf6c, 401, V_DRAW}, {0xabd1b8, 464, V_BARRIER}, {0xabd2cc, 464, V_BARRIER},
    {0xabd2f0, 464, V_BARRIER}, {0xabd314, 464, V_BARRIER}, {0xabd338, 464, V_BARRIER}, {0xabd360, 464, V_BARRIER},
    {0xabd420, 401, V_DRAW}, {0xabd6a0, 464, V_BARRIER}, {0xabd6c4, 464, V_BARRIER}, {0xabd6e8, 464, V_BARRIER},
    {0xabd70c, 464, V_BARRIER}, {0xabd730, 464, V_BARRIER}, {0xabd754, 464, V_BARRIER}, {0xabd82c, 464, V_BARRIER},
    {0xabd8dc, 464, V_BARRIER}, {0xabd8fc, 464, V_BARRIER}, {0xabd924, 464, V_BARRIER}, {0xabd960, 455, V_REDUNDANT},
    {0xabd9ac, 455, V_REDUNDANT}, {0xabe29c, 374, V_REDUNDANT}, {0xabe324, 375, V_REDUNDANT}, {0xabe33c, 430, V_REDUNDANT},
    {0xabe354, 429, V_REDUNDANT}, {0xabe36c, 431, V_REDUNDANT}, {0xabe3f8, 376, V_REDUNDANT}, {0xabe42c, 377, V_REDUNDANT},
    {0xabe4b4, 379, V_REDUNDANT}, {0xabe5cc, 401, V_DRAW}, {0xabe660, 455, V_REDUNDANT}, {0xabe8ec, 401, V_DRAW},
    {0xabe930, 455, V_REDUNDANT}, {0xabf164, 401, V_DRAW}, {0xabf1c8, 455, V_REDUNDANT}, {0xabf870, 401, V_DRAW},
    {0xabf8fc, 455, V_REDUNDANT}, {0xac1750, 464, V_BARRIER}, {0xac1778, 464, V_BARRIER}, {0xac1d24, 464, V_BARRIER},
    {0xac1d74, 464, V_BARRIER}, {0xac1dc4, 464, V_BARRIER}, {0xac3250, 464, V_BARRIER}, {0xac350c, 464, V_BARRIER},
    {0xac355c, 464, V_BARRIER}, {0xac35ac, 464, V_BARRIER}, {0xac6750, 464, V_BARRIER}, {0xac677c, 464, V_BARRIER},
    {0xac67a8, 464, V_BARRIER}, {0xac67d4, 464, V_BARRIER}, {0xac6810, 464, V_BARRIER}, {0xac70e4, 464, V_BARRIER},
    {0xac7110, 464, V_BARRIER}, {0xac713c, 464, V_BARRIER}, {0xac7168, 464, V_BARRIER}, {0xac7190, 464, V_BARRIER},
    {0xac71c0, 464, V_BARRIER}, {0xac71e8, 464, V_BARRIER}, {0xac9a54, 464, V_BARRIER}, {0xaf31fc, 401, V_DRAW},
    {0xaf3260, 455, V_REDUNDANT}, {0xafaa78, 374, V_REDUNDANT}, {0xafaaf0, 374, V_REDUNDANT}, {0xb0a8f4, 415, V_VPSC},
    {0xb0a914, 418, V_VPSC}, {0xb0abb4, 464, V_BARRIER}, {0xb0ac3c, 464, V_BARRIER}, {0xb0ace8, 415, V_VPSC}, {0xb0ad0c, 418, V_VPSC},
    {0xb0aebc, 400, V_DRAW}, {0xb0aee0, 464, V_BARRIER}, {0xb0afdc, 374, V_REDUNDANT}, {0xb0b03c, 375, V_REDUNDANT},
    {0xb0b054, 430, V_REDUNDANT}, {0xb0b06c, 429, V_REDUNDANT}, {0xb0b084, 431, V_REDUNDANT}, {0xb0b164, 455, V_REDUNDANT},
    {0xb15500, 415, V_VPSC}, {0xb15524, 418, V_VPSC}, {0xb1ce34, 464, V_BARRIER}, {0xb1ef54, 464, V_BARRIER},
    {0xb1ef78, 464, V_BARRIER}, {0xb1ef98, 464, V_BARRIER}, {0xb1efc0, 464, V_BARRIER}, {0xb1efe4, 464, V_BARRIER},
    {0xb1f0e0, 464, V_BARRIER}, {0xb1f104, 464, V_BARRIER}, {0xb1f128, 464, V_BARRIER}, {0xb205fc, 415, V_VPSC},
    {0xb2061c, 418, V_VPSC}, {0xb20a08, 415, V_VPSC}, {0xb20a28, 418, V_VPSC}, {0xb23e18, 375, V_REDUNDANT},
    {0xb23e30, 430, V_REDUNDANT}, {0xb23e48, 429, V_REDUNDANT}, {0xb23e60, 431, V_REDUNDANT}, {0xb23f08, 376, V_REDUNDANT},
    {0xb23f40, 377, V_REDUNDANT}, {0xb23fdc, 379, V_REDUNDANT}, {0xb2413c, 456, V_REDUNDANT}, {0xb24174, 455, V_REDUNDANT},
    {0xb24254, 379, V_REDUNDANT}, {0xb2431c, 379, V_REDUNDANT}, {0xb7e4b4, 452, V_QUERY}, {0xb7e4dc, 452, V_QUERY},
    {0xb8774c, 452, V_QUERY}, {0xb87778, 452, V_QUERY}, {0xb89678, 401, V_DRAW}, {0xb896b4, 452, V_QUERY}, {0xb896f4, 452, V_QUERY},
    {0xb8985c, 401, V_DRAW}, {0xb898c4, 455, V_REDUNDANT}, {0xb898f4, 455, V_REDUNDANT}, {0xd1b124, 415, V_VPSC},
    {0xd1b144, 418, V_VPSC}, {0xd1b244, 379, V_REDUNDANT}, {0xd1b314, 400, V_DRAW}, {0xd1b5e4, 376, V_REDUNDANT},
    {0xd1b618, 377, V_REDUNDANT}, {0xd1b680, 374, V_REDUNDANT}, {0xd1b6e4, 375, V_REDUNDANT}, {0xd1b6fc, 430, V_REDUNDANT},
    {0xd1b714, 429, V_REDUNDANT}, {0xd1b72c, 431, V_REDUNDANT}, {0xd1b7c0, 455, V_REDUNDANT}, {0xd20078, 415, V_VPSC},
    {0xd20098, 418, V_VPSC}, {0xd20104, 457, V_RT}, {0xd20190, 457, V_RT}, {0xd201e8, 418, V_VPSC}, {0xd20e34, 464, V_BARRIER},
    {0xd20e4c, 464, V_BARRIER}, {0x1241170, 401, V_DRAW}, {0x12411c8, 455, V_REDUNDANT}, {0x1335d6c, 401, V_DRAW},
    {0x1335de4, 455, V_REDUNDANT}, {0x1422404, 464, V_BARRIER}, {0x1422b74, 448, V_CLEAR}, {0x1422bc4, 464, V_BARRIER},
    {0x1422c98, 448, V_CLEAR}, {0x1423144, 464, V_BARRIER}, {0x142316c, 464, V_BARRIER}, {0x1423190, 464, V_BARRIER},
    {0x14248dc, 464, V_BARRIER}, {0x142d004, 379, V_REDUNDANT}, {0x142d0fc, 456, V_REDUNDANT}, {0x142d1cc, 376, V_REDUNDANT},
    {0x142d204, 375, V_REDUNDANT}, {0x142d21c, 430, V_REDUNDANT}, {0x142d234, 429, V_REDUNDANT}, {0x142d24c, 431, V_REDUNDANT},
    {0x142d300, 379, V_REDUNDANT}, {0x142d388, 455, V_REDUNDANT}, {0x142d3bc, 377, V_REDUNDANT}, {0x142d468, 379, V_REDUNDANT},
};

// ---- statistics: every drop / unexpected condition has a counter; the first 16 of each print one red line ----
enum {
    A_ELEMENTS, A_ELEMENTS_INST, A_ARRAYS, A_VP_NOTE, A_VP_OVR, A_SC_VP, A_SC_OVR_CACHE, A_SC_OVR_BEHIND, A_CLEAR_TEX, A_CLEAR_DS, A_RT,
    A_SINK_SUBMIT, A_SINK_END, A_SINK_WAIT, A_FWD, A_FWD_HOST, A_REPOINT,
    // drops / unexpected (red)
    D_NO_R, D_DRAW_DROP, D_PRIM, D_PRIM_FIX, D_IB_NONE, D_IB_TYPE, D_IB_ADDR, D_INSTANCE_BASE, D_TEX_UNKNOWN, D_TEX_LEVEL, D_CLEAR_MASK,
    D_CLEAR_REGION, D_DS_NONE, D_RT_UNKNOWN, D_RT_N, D_UNKNOWN_SITE, D_HASH_FULL, D_FWD_NULL, D_TABLE,
    C_N
};
static const char *const k_why[C_N] = {
    [D_NO_R] = "no renderer ([main+0x2bbdc68] = 0): call ignored",
    [D_DRAW_DROP] = "the draw module dropped a direct draw (its census line names the reason)",
    [D_PRIM] = "direct draw with an NVN draw mode that is no engine primitive (table main+0x1f58640): dropped",
    [D_PRIM_FIX] = "direct draw: the NVN draw mode differs from the cache r+0x1718 (primitive taken from the mode)",
    [D_IB_NONE] = "DrawElements: the bound index buffer r+0x17c8 is not a live native buffer: dropped",
    [D_IB_TYPE] = "DrawElements: the NVN index type differs from the bound index buffer's index size: dropped",
    [D_IB_ADDR] = "DrawElements: the NVN address is not inside the bound index buffer (pseudo address of its block): dropped",
    [D_INSTANCE_BASE] = "DrawElementsInstanced with a base vertex / base instance (no backend argument): drawn without them",
    [D_TEX_UNKNOWN] = "ClearTexture: the NVN texture object is unknown to the backend: dropped",
    [D_TEX_LEVEL] = "ClearTexture of a level other than 0 / a layer outside the texture: dropped",
    [D_CLEAR_MASK] = "ClearTexture with a channel mask other than 0xf: all channels cleared",
    [D_CLEAR_REGION] = "ClearTexture region with z offset / depth other than 0 / 1: the view's layer cleared",
    [D_DS_NONE] = "ClearDepthStencil: no depth target bound in the backend: nothing cleared",
    [D_RT_UNKNOWN] = "SetRenderTargets: an NVN texture object unknown to the backend: that slot left unbound",
    [D_RT_N] = "SetRenderTargets with more than 8 colours: the first 8 taken",
    [D_UNKNOWN_SITE] = "direct NVN call from a site outside the verdict table",
    [D_HASH_FULL] = "call-site table full: call counted by slot only",
    [D_FWD_NULL] = "forwarded call to a slot the game's table held no function for: ignored",
    [D_TABLE] = "the game's NVN table holds a null entry for a repointed slot (repointed anyway)",
};
static uint64_t g_c[C_N], g_vc[V_N], g_slot_n[TABLE_SLOTS];
static uint32_t g_red[C_N];
static inline void cnt(int k) { __atomic_add_fetch(&g_c[k], 1, __ATOMIC_RELAXED); }
static void red(int k, uint64_t a, uint64_t b, uint64_t c) {
    cnt(k);
    if (__atomic_add_fetch(&g_red[k], 1, __ATOMIC_RELAXED) <= 16)
        vkn_logf(CMD_RED "cmd: %s (values %#llx %#llx %#llx)", k_why[k], (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
}
static inline uint64_t ld(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

// ---- slot names / kinds by index ----
static const char *g_sname[TABLE_SLOTS];
static uint8_t g_skind[TABLE_SLOTS];
static uint64_t g_orig[TABLE_SLOTS];          // what the game's table held (HLE NVN's thunk) before the repointing
static uint64_t g_text_end;                   // main's text size: a return address below it is engine code

static const cmd_known *known_at(uint32_t ra, int slot) {
    size_t lo = 0, hi = sizeof k_known / sizeof k_known[0];
    while (lo < hi) { const size_t m = (lo + hi) / 2; if (k_known[m].ra < ra) lo = m + 1; else hi = m; }
    return lo < sizeof k_known / sizeof k_known[0] && k_known[lo].ra == ra && k_known[lo].slot == slot ? &k_known[lo] : NULL;
}

// ---- call sites: open addressing (key = (return offset + 1) << 12 | slot), counts since start and at the last census ----
enum { CMD_HASH = 2048 };
static uint64_t g_key[CMD_HASH], g_n[CMD_HASH], g_prev[CMD_HASH];
static void site_new(uint32_t off, int slot) {
    if (off == OFF_HOST) return;
    const cmd_known *k = known_at(off, slot);
    if (k) return;
    cnt(D_UNKNOWN_SITE);
    if (__atomic_add_fetch(&g_red[D_UNKNOWN_SITE], 1, __ATOMIC_RELAXED) <= 16)
        vkn_logf(CMD_RED "cmd: first direct NVN call nvn%s from main+%#x, a site outside the verdict table (the UI part's, or a replaced function's path that still reaches NVN)",
                 g_sname[slot] ? g_sname[slot] : "?", off);
}
static void site_count(uint32_t off, int slot) {
    const uint64_t key = ((uint64_t)off + 1) << 12 | (uint64_t)slot;
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 53) & (CMD_HASH - 1);
    for (int n = 0; n < CMD_HASH; n++, h = (h + 1) & (CMD_HASH - 1)) {
        uint64_t cur = __atomic_load_n(&g_key[h], __ATOMIC_ACQUIRE);
        if (!cur) {
            uint64_t z = 0;
            if (__atomic_compare_exchange_n(&g_key[h], &z, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { site_new(off, slot); cur = key; }
            else cur = z;
        }
        if (cur == key) { __atomic_add_fetch(&g_n[h], 1, __ATOMIC_RELAXED); return; }
    }
    red(D_HASH_FULL, off, (uint64_t)slot, 0);
}

// ---- viewport / scissor overrides: what a direct call set behind the caches. A viewport override holds while the viewport cache
// r+0x171c is unchanged (packet 0x12 and the pass part's full-target resets change it when they call NVN, nothing else does); a
// scissor override while neither cache changed (both kinds of events set NVN's scissor); both end with the recording (a new command
// buffer has no state). Recording thread only. ----
static struct { int on; uint32_t serial; int32_t rect[4]; uint8_t vp[16], sc[16]; } g_ovr_vp, g_ovr_sc;
static void ovr_set(uint64_t r, int which, const int32_t xywh[4]) {
    __typeof__(g_ovr_vp) *o = which ? &g_ovr_sc : &g_ovr_vp;
    o->on = 1; o->serial = vkn_cb_serial(); memcpy(o->rect, xywh, sizeof o->rect);
    memcpy(o->vp, (const void *)(uintptr_t)(r + R_VP), 16); memcpy(o->sc, (const void *)(uintptr_t)(r + R_SC), 16);
}
static int ovr_live(uint64_t r, int which) {
    __typeof__(g_ovr_vp) *o = which ? &g_ovr_sc : &g_ovr_vp;
    if (!o->on) return 0;
    if (o->serial != vkn_cb_serial() || memcmp(o->vp, (const void *)(uintptr_t)(r + R_VP), 16) || (which && memcmp(o->sc, (const void *)(uintptr_t)(r + R_SC), 16))) { o->on = 0; return 0; }
    return 1;
}
// The rectangles NVN would have at a draw where a direct call set them behind the engine's caches. Returns bit 0: vp = the viewport
// {x, y, w, h}; bit 1: sc = the scissor {x, y, w, h}; 0 = the caches and the exec part's model are right. For the draw part's
// dm_vkn_draw_fill (call it after filling: an override replaces in->viewport / in->scissor); this file's draws apply it themselves.
int dm_vkn_cmd_override(uint64_t r, float vp[4], int32_t sc[4]) {
    if (!r) return 0;
    int m = 0;
    if (ovr_live(r, 0)) { if (vp) for (int i = 0; i < 4; i++) vp[i] = (float)g_ovr_vp.rect[i]; m |= 1; }
    if (ovr_live(r, 1)) { if (sc) memcpy(sc, g_ovr_sc.rect, sizeof g_ovr_sc.rect); m |= 2; }
    return m;
}

// ---- actions ----
static int prim_of_mode(uint64_t r, uint32_t mode, uint8_t *prim) {   // 1 = ok
    if (vkn_rd32(r + R_NVN_PRIM) == mode) { *prim = vkn_rd8(r + R_PRIM); return 1; }
    const uint64_t tab = (uint64_t)(uintptr_t)(dm_vkn_base() + M_PRIM_TAB);
    for (uint32_t p = 1; p <= 7; p++) if (vkn_rd32(tab + 4ull * (p - 1)) == mode) { red(D_PRIM_FIX, mode, vkn_rd32(r + R_NVN_PRIM), p); *prim = (uint8_t)p; return 1; }
    red(D_PRIM, mode, vkn_rd32(r + R_NVN_PRIM), vkn_rd8(r + R_PRIM));
    return 0;
}
static void fill(uint64_t r, vkn_draw_in *in) {
    dm_vkn_draw_fill(in);
    float vp[4]; int32_t sc[4];
    const int m = dm_vkn_cmd_override(r, vp, sc);
    if (m & 1) memcpy(in->viewport, vp, sizeof vp);
    if (m & 2) memcpy(in->scissor, sc, sizeof sc);
}
// nvnCommandBufferDrawElements(cb, mode, type, count, address) / ...Instanced(cb, mode, type, count, address, base vertex, base
// instance, instances).
static void act_elements(uint32_t mode, uint32_t type, int32_t count, uint64_t addr, int32_t basev, int32_t basei, int32_t inst) {
    const uint64_t r = vkn_renderer();
    if (!r) { red(D_NO_R, S_DRAW_ELEMENTS, mode, count); return; }
    if (count <= 0 || inst <= 0) return;   // NVN draws nothing
    vkn_draw_in in; fill(r, &in);
    if (!prim_of_mode(r, mode, &in.prim)) return;
    vkn_buf_info ib;
    if (!in.ib || vkn_buf_lookup(VKN_BUF_IB, in.ib, &ib) || !ib.blk.size) { red(D_IB_NONE, in.ib, addr, (uint64_t)count); return; }
    const uint32_t isz = type == 2 ? 4 : type == 1 ? 2 : type == 0 ? 1 : 0;   // NVN index types 0 u8, 1 u16, 2 u32
    if (!isz || isz != ib.index_size) { red(D_IB_TYPE, in.ib, type, ib.index_size); return; }
    const uint64_t va = vkn_buf_va(&ib.blk);
    if (addr < va || (addr - va) % isz || (addr - va) / isz + (uint64_t)count > ib.size / isz) { red(D_IB_ADDR, in.ib, addr, va); return; }
    if (basev || basei) red(D_INSTANCE_BASE, (uint64_t)(uint32_t)basev, (uint64_t)(uint32_t)basei, in.ib);
    cnt(inst > 1 || basev || basei ? A_ELEMENTS_INST : A_ELEMENTS);
    if (vkn_draw_indexed(&in, (uint32_t)((addr - va) / isz), (uint32_t)count, (uint32_t)inst)) cnt(D_DRAW_DROP);
}
static void act_arrays(uint32_t mode, int32_t first, int32_t count) {   // nvnCommandBufferDrawArrays(cb, mode, first, count)
    const uint64_t r = vkn_renderer();
    if (!r) { red(D_NO_R, S_DRAW_ARRAYS, mode, count); return; }
    if (count <= 0 || first < 0) return;
    vkn_draw_in in; fill(r, &in);
    if (!prim_of_mode(r, mode, &in.prim)) return;
    cnt(A_ARRAYS);
    if (vkn_draw_arrays(&in, (uint32_t)first, (uint32_t)count)) cnt(D_DRAW_DROP);
}
// nvnCommandBufferSetViewport(cb, x, y, w, h): the callers wrote r+0x171c {w, h, x, y} before (see the verdicts); NVN's scissor is
// untouched by it. A rectangle that is not the cache's is an override.
static void act_viewport(int32_t x, int32_t y, int32_t w, int32_t h) {
    const uint64_t r = vkn_renderer();
    if (!r) { red(D_NO_R, S_VIEWPORT, (uint64_t)(uint32_t)x, (uint64_t)(uint32_t)y); return; }
    const int32_t c[4] = {(int32_t)vkn_rd32(r + R_VP + 8), (int32_t)vkn_rd32(r + R_VP + 12), (int32_t)vkn_rd32(r + R_VP), (int32_t)vkn_rd32(r + R_VP + 4)};
    const int32_t v[4] = {x, y, w, h};
    if (!memcmp(c, v, sizeof v)) { g_ovr_vp.on = 0; return; }
    cnt(A_VP_OVR);
    ovr_set(r, 0, v);
}
// nvnCommandBufferSetScissor(cb, x, y, w, h): the viewport rectangle (every caller's pair) -> the exec part's viewport note (its model
// then answers "scissor = viewport", what NVN has); anything else -> the scissor override.
static void act_scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
    const uint64_t r = vkn_renderer();
    if (!r) { red(D_NO_R, S_SCISSOR, (uint64_t)(uint32_t)x, (uint64_t)(uint32_t)y); return; }
    const int32_t vp[4] = {(int32_t)vkn_rd32(r + R_VP + 8), (int32_t)vkn_rd32(r + R_VP + 12), (int32_t)vkn_rd32(r + R_VP), (int32_t)vkn_rd32(r + R_VP + 4)};
    const int32_t s[4] = {x, y, w, h};
    if (!memcmp(vp, s, sizeof s) && !ovr_live(r, 0)) { cnt(A_SC_VP); g_ovr_sc.on = 0; dm_vkn_exec_viewport_note(); return; }
    const int32_t ltrb[4] = {x, y, x + w, y + h};
    cnt(memcmp(ltrb, (const void *)(uintptr_t)(r + R_SC), sizeof ltrb) ? A_SC_OVR_BEHIND : A_SC_OVR_CACHE);
    ovr_set(r, 1, s);
}
// Levels / layers of an NVN view argument (HLE NVN's view_t in guest memory; NULL = level 0, layer 0).
static void view_of(uint64_t v, uint32_t *layer, uint32_t *level) {
    *layer = 0; *level = 0;
    if (!v) return;
    const view_t *vw = (const view_t *)(uintptr_t)v;
    if (vw->set_levels) *level = (uint32_t)vw->base_level;
    if (vw->set_layers) *layer = (uint32_t)vw->min_layer;
}
// nvnCommandBufferClearTexture(cb, texture, view, region {x, y, z, w, h, d}, float rgba[4], mask).
static void act_clear_tex(uint64_t tex, uint64_t view, uint64_t region, uint64_t color, uint32_t mask) {
    vkn_tex *t = tex ? vkn_tex_by_cookie(tex) : NULL;
    if (!t || !color) { red(D_TEX_UNKNOWN, tex, view, color); return; }
    uint32_t layer, level; view_of(view, &layer, &level);
    if (level || layer >= t->layers) { red(D_TEX_LEVEL, tex, level, layer); return; }
    if ((mask & 0xf) != 0xf) red(D_CLEAR_MASK, tex, mask, 0);
    float rgba[4]; memcpy(rgba, (const void *)(uintptr_t)color, sizeof rgba);
    int32_t rect[4]; const int32_t *rp = NULL;
    if (region) {
        int32_t g[6]; memcpy(g, (const void *)(uintptr_t)region, sizeof g);
        if (g[2] || g[5] != 1) red(D_CLEAR_REGION, tex, (uint64_t)(uint32_t)g[2], (uint64_t)(uint32_t)g[5]);
        if (g[3] <= 0 || g[4] <= 0) return;   // an empty region clears nothing
        rect[0] = g[0]; rect[1] = g[1]; rect[2] = g[3]; rect[3] = g[4]; rp = rect;
    }
    cnt(A_CLEAR_TEX);
    vkn_pass_clear_color(t, layer, rgba, rp);
}
// nvnCommandBufferClearDepthStencil(cb, float depth, NVNboolean depth mask, int stencil, int stencil mask): the bound depth target.
static void act_clear_ds(float depth, uint32_t dmask, int32_t stencil, int32_t smask) {
    const int what = ((dmask & 0xff) ? 1 : 0) | (smask ? 2 : 0);
    if (!what) return;
    vkn_tex *t = vkn_pass_depth();
    if (!t) { red(D_DS_NONE, dmask, (uint64_t)(uint32_t)smask, 0); return; }
    cnt(A_CLEAR_DS);
    vkn_pass_clear_depth(t, what, depth, (uint32_t)stencil);
}
// nvnCommandBufferSetRenderTargets(cb, n, const NVNtexture *const *colours, const NVNtextureView *const *views, depth, depth view).
static void act_targets(int32_t n, uint64_t cols, uint64_t views, uint64_t depth, uint64_t dview) {
    (void)dview;
    if (n < 0) n = 0;
    if (n > VKN_PASS_COLORS) { red(D_RT_N, (uint64_t)n, cols, depth); n = VKN_PASS_COLORS; }
    vkn_tex *ct[VKN_PASS_COLORS]; uint32_t L[VKN_PASS_COLORS], M[VKN_PASS_COLORS];
    memset(ct, 0, sizeof ct); memset(L, 0, sizeof L); memset(M, 0, sizeof M);
    for (int i = 0; i < n && cols; i++) {
        const uint64_t o = vkn_rd64(cols + 8ull * (uint64_t)i);
        if (!o) continue;
        vkn_tex *t = vkn_tex_by_cookie(o);
        if (!t) { red(D_RT_UNKNOWN, o, (uint64_t)i, 0); continue; }
        uint32_t l, m; view_of(views ? vkn_rd64(views + 8ull * (uint64_t)i) : 0, &l, &m);
        if (l >= t->layers || m >= t->mips) { red(D_RT_UNKNOWN, o, l, m); continue; }
        ct[i] = t; L[i] = l; M[i] = m;
    }
    vkn_tex *dt = NULL;
    if (depth && !(dt = vkn_tex_by_cookie(depth))) red(D_RT_UNKNOWN, depth, 0xd, 0);
    cnt(A_RT);
    vkn_pass_targets((uint32_t)n, ct, L, M, dt);
}

// ---- the entry: every repointed slot's host function lands here ----
static int creation_caller(uint32_t off) { return (off >= F_CREATE_DEV && off < F_CREATE_DEV_END) || (off >= F_CREATE_CTX && off < F_CREATE_CTX_END); }
static uint64_t cmd_entry(int slot, uint64_t ra, VKN_A) {
    const uint64_t base = (uint64_t)(uintptr_t)dm_vkn_base();
    const uint32_t off = ra >= base && ra - base < g_text_end ? (uint32_t)(ra - base) : OFF_HOST;
    __atomic_add_fetch(&g_slot_n[slot], 1, __ATOMIC_RELAXED);
    site_count(off, slot);
    if (off != OFF_HOST) { const cmd_known *k = known_at(off, slot); if (k) __atomic_add_fetch(&g_vc[k->v], 1, __ATOMIC_RELAXED); }
    if (off == OFF_HOST || creation_caller(off) || g_skind[slot] == K_FWD) {   // HLE NVN answers
        cnt(off == OFF_HOST ? A_FWD_HOST : A_FWD);
        const vkn_fn f = (vkn_fn)(uintptr_t)g_orig[slot];
        if (!f) { red(D_FWD_NULL, (uint64_t)slot, off, 0); return 0; }
        return f(VKN_F);
    }
    switch (slot) {
    case S_DRAW_ELEMENTS: act_elements((uint32_t)a1, (uint32_t)a2, (int32_t)a3, a4, 0, 0, 1); return 0;
    case S_DRAW_ELEMENTS_INST: act_elements((uint32_t)a1, (uint32_t)a2, (int32_t)a3, a4, (int32_t)a5, (int32_t)a6, (int32_t)a7); return 0;
    case S_DRAW_ARRAYS: act_arrays((uint32_t)a1, (int32_t)a2, (int32_t)a3); return 0;
    case S_VIEWPORT: act_viewport((int32_t)a1, (int32_t)a2, (int32_t)a3, (int32_t)a4); return 0;
    case S_SCISSOR: act_scissor((int32_t)a1, (int32_t)a2, (int32_t)a3, (int32_t)a4); return 0;
    case S_CLEAR_TEX: act_clear_tex(a1, a2, a3, a4, (uint32_t)a5); return 0;
    case S_CLEAR_DS: { uint64_t bits; memcpy(&bits, &d0, 8); const uint32_t lo = (uint32_t)bits; float z; memcpy(&z, &lo, 4);   // s0 = low half of v0
                       act_clear_ds(z, (uint32_t)a1, (int32_t)a2, (int32_t)a3); return 0; }
    case S_RT: act_targets((int32_t)a1, a2, a3, a4, a5); return 0;
    case S_END_REC: cnt(A_SINK_END); return 0;                            // NVNcommandHandle 0: the submit that gets it is dropped too
    case S_SUBMIT: cnt(A_SINK_SUBMIT); return 0;
    case S_SYNC_WAIT: cnt(A_SINK_WAIT); return NVN_SYNC_SATISFIED;        // the frame part's EndFrame waited for the GPU already
    case S_QUEUE_WAIT_SYNC: return 1;                                     // NVN_TRUE
    default: return 0;                                                    // counted no-op (void)
    }
}
// Each slot function is entered through its host-stack stub (dm_vkn_stack_stub, made at registration): the engine caller's return
// address is the one the stub kept (dm_vkn_caller), this function's own is the stub's veneer.
#define X_FN(i, n, k) VKN_HOOK cmd_s##i(VKN_A) { return cmd_entry(i, dm_vkn_caller(), VKN_F); }
CMD_SLOTS(X_FN)
#undef X_FN
typedef struct { uint16_t idx; uint8_t kind; const char *name; void *fn; } cmd_slot;
#define X_ROW(i, n, k) {i, k, n, (void *)cmd_s##i},
static const cmd_slot k_slots[] = { CMD_SLOTS(X_ROW) };
#undef X_ROW

// ---- the repointing: when device init 0x938330 returns. Hooked there, not at 0x934d70: 0x934d70 calls GetProcAddress through x19
// with x8 unwritten, and a hook does not forward x8 (tools/dos2_nr_check.py rejects that site); 0x938330 forwards cleanly. ----
static vkn_fn g_o_init;
static int g_fills;
void dm_vkn_glue_repoint(void) __attribute__((weak));
static void *g_stub[TABLE_SLOTS];   // the host-stack stub of each repointed slot's function (dm_vkn_cmd_register)
static void repoint(uint64_t init) {
    uint64_t *tab = (uint64_t *)(void *)(dm_vkn_base() + M_TABLE);
    int n = 0, nul = 0;
    for (size_t i = 0; i < sizeof k_slots / sizeof k_slots[0]; i++) {
        const cmd_slot *s = &k_slots[i];
        const uint64_t cur = __atomic_load_n(&tab[s->idx], __ATOMIC_ACQUIRE), mine = (uint64_t)(uintptr_t)g_stub[s->idx];
        if (!mine) continue;   // (no stub page: the slot stays HLE NVN; red line at registration)
        if (cur == mine) continue;
        if (!cur) { nul++; red(D_TABLE, s->idx, init, 0); }
        g_orig[s->idx] = cur;
        __atomic_store_n(&tab[s->idx], mine, __ATOMIC_RELEASE);
        n++;
    }
    if (dm_vkn_glue_repoint) dm_vkn_glue_repoint();   // hle_dos2_vkn_glue.c: nvnTextureGetStride noted (textures written through their storage)
    cnt(A_REPOINT);
    vkn_logf("cmd: game NVN table main+%#x after device init %d (object %#llx): %d of %zu command / queue / sync entries repointed to the native "
             "front end (%d were null); creation-time entries stay HLE NVN", M_TABLE, ++g_fills, (unsigned long long)init, n, sizeof k_slots / sizeof k_slots[0], nul);
}
VKN_HOOK cmd_h_init(VKN_A) {   // 0x938330(init object) -> u8 ok: device, queue, pools, contexts (creation-time NVN: HLE NVN as before)
    const uint64_t rc = g_o_init(VKN_F);
    repoint(a0);
    return rc;
}

// ---- census ----
uint64_t dm_vkn_cmd_calls(void) { uint64_t t = 0; for (int i = 0; i < TABLE_SLOTS; i++) t += ld(&g_slot_n[i]); return t; }
static void cmd_census(void) {
    enum { TOP = 8 };
    int top[TOP]; uint64_t topn[TOP], win = 0, unk_win = 0;
    for (int k = 0; k < TOP; k++) { top[k] = -1; topn[k] = 0; }
    char unk[400]; int u = 0; unk[0] = 0;
    for (int h = 0; h < CMD_HASH; h++) {
        const uint64_t key = ld(&g_key[h]);
        if (!key) continue;
        const uint64_t n = ld(&g_n[h]), d = n - g_prev[h]; g_prev[h] = n;
        if (!d) continue;
        win += d;
        const uint32_t off = (uint32_t)((key >> 12) - 1); const int slot = (int)(key & 0xfff);
        if (off != OFF_HOST && !known_at(off, slot)) {
            unk_win += d;
            if (u < (int)sizeof unk - 96) u += snprintf(unk + u, sizeof unk - (size_t)u, " main+%#x (%s) x %llu", off, g_sname[slot] ? g_sname[slot] : "?", (unsigned long long)d);
        }
        for (int k = 0; k < TOP; k++) if (d > topn[k]) {   // insertion into the top list
            for (int j = TOP - 1; j > k; j--) { top[j] = top[j - 1]; topn[j] = topn[j - 1]; }
            top[k] = h; topn[k] = d; break;
        }
    }
    char tops[700]; int t = 0; tops[0] = 0;
    for (int k = 0; k < TOP && top[k] >= 0 && t < (int)sizeof tops - 96; k++) {
        const uint64_t key = ld(&g_key[top[k]]); const uint32_t off = (uint32_t)((key >> 12) - 1); const int slot = (int)(key & 0xfff);
        if (off == OFF_HOST) t += snprintf(tops + t, sizeof tops - (size_t)t, "%s host (%s) x %llu", k ? "," : "", g_sname[slot] ? g_sname[slot] : "?", (unsigned long long)topn[k]);
        else t += snprintf(tops + t, sizeof tops - (size_t)t, "%s main+%#x (%s) x %llu", k ? "," : "", off, g_sname[slot] ? g_sname[slot] : "?", (unsigned long long)topn[k]);
    }
    vkn_logf("stray NVN calls: %llu in this window (%llu total), top sites%s", (unsigned long long)win, (unsigned long long)dm_vkn_cmd_calls(), t ? tops : " none");
    if (unk_win) vkn_logf(CMD_RED "stray NVN calls outside the verdict table: %llu in this window:%s", (unsigned long long)unk_win, unk);
#define U(k) (unsigned long long)ld(&g_c[k])
    char vs[300]; int v = 0; vs[0] = 0;
    for (int i = 0; i < V_N; i++) if (ld(&g_vc[i])) v += snprintf(vs + v, sizeof vs - (size_t)v, " %s %llu", k_vname[i], (unsigned long long)ld(&g_vc[i]));
    vkn_logf("cmd: by verdict:%s; actions: DrawElements %llu (+%llu instanced), DrawArrays %llu, viewport overrides %llu, scissor = viewport %llu, "
             "scissor overrides %llu (cache written %llu), ClearTexture %llu, ClearDepthStencil %llu, SetRenderTargets %llu; frame sinks: EndRecording %llu, "
             "submits %llu, SyncWait %llu; forwarded to HLE NVN %llu (+%llu from host code); table fills %llu",
             v ? vs : " none", U(A_ELEMENTS), U(A_ELEMENTS_INST), U(A_ARRAYS), U(A_VP_OVR), U(A_SC_VP), U(A_SC_OVR_CACHE) + U(A_SC_OVR_BEHIND), U(A_SC_OVR_CACHE),
             U(A_CLEAR_TEX), U(A_CLEAR_DS), U(A_RT), U(A_SINK_END), U(A_SINK_SUBMIT), U(A_SINK_WAIT), U(A_FWD), U(A_FWD_HOST), U(A_REPOINT));
    uint64_t drops = 0; for (int i = D_NO_R; i < C_N; i++) drops += ld(&g_c[i]);
    if (!drops) return;
    static const char *const nm[C_N] = {[D_NO_R] = "no-renderer", [D_DRAW_DROP] = "draw-dropped", [D_PRIM] = "prim", [D_PRIM_FIX] = "prim-mismatch",
        [D_IB_NONE] = "ib-none", [D_IB_TYPE] = "ib-type", [D_IB_ADDR] = "ib-address", [D_INSTANCE_BASE] = "base-vertex", [D_TEX_UNKNOWN] = "clear-tex",
        [D_TEX_LEVEL] = "clear-level", [D_CLEAR_MASK] = "clear-mask", [D_CLEAR_REGION] = "clear-region", [D_DS_NONE] = "ds-none", [D_RT_UNKNOWN] = "rt-unknown",
        [D_RT_N] = "rt-n", [D_UNKNOWN_SITE] = "unknown-sites", [D_HASH_FULL] = "sites-full", [D_FWD_NULL] = "fwd-null", [D_TABLE] = "table-null"};
    char line[600]; int k = 0; line[0] = 0;
    for (int i = D_NO_R; i < C_N && k < (int)sizeof line - 48; i++) if (ld(&g_c[i])) k += snprintf(line + k, sizeof line - (size_t)k, " %s %llu", nm[i], U(i));
    vkn_logf(CMD_RED "cmd: dropped / unexpected:%s", line);
#undef U
}

// ---- registration (hle_dos2_vkn.c calls it after the other parts) ----
void dm_vkn_cmd_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;   // mode 2 keeps the game's table as HLE NVN filled it
    module_t *m = dn_main();
    g_text_end = m && m->text_end ? m->text_end : 0x1e80000;   // (main.raw: text ends at 0x1e80000)
    int nostub = 0;
    for (size_t i = 0; i < sizeof k_slots / sizeof k_slots[0]; i++) {
        g_sname[k_slots[i].idx] = k_slots[i].name; g_skind[k_slots[i].idx] = k_slots[i].kind;
        // The slot functions run Vulkan (draws, clears, targets) for a guest caller: entered on the host stack like every site.
        if (!(g_stub[k_slots[i].idx] = dm_vkn_stack_stub(k_slots[i].fn))) nostub++;
    }
    if (nostub) vkn_logf(CMD_RED "cmd: %d slot functions without a host-stack stub: those slots stay HLE NVN", nostub);
    // First 4 instructions of 0x938330 (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_init[4] = {0xd10583ff /* sub sp,sp,#0x160 */, 0xa9106ffc, 0xa91167fa, 0xa9125ff8};
    dm_vkn_site(0x938330, k_init, "DeviceInit", (void *)cmd_h_init, &g_o_init, VKN_M_NATIVE);
    dm_vkn_census(cmd_census);
}
#endif
