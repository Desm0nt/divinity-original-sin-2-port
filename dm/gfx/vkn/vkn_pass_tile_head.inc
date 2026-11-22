// [DOS2] VK-NATIVE P10: types, state and prototypes of vkn_pass_tile.inc (included early by vkn_pass.c: end_open and the entry points use them).
enum { TL_NONE = 0, TL_REC, TL_SUSP };
enum { TL_SUBS = VKN_MP_SUBS, TL_ATTS = VKN_MP_ATTS, TL_SECS = 16, TL_TWINS = 64 };
// why a chain did not begin (refusals) / did not complete (misses)
enum { TLR_HIST, TLR_BAN, TLR_PRIME, TLR_HINT, TLR_QUERY, TLR_OFFFRAME, TLR_TARGET, TLR_PASS, TLR_COOL, TLR_LAZY, TLR_N };
static const char *const k_tlr[TLR_N] = {"no learned chain", "banned after misses", "pipelines not built yet", "a UI area hint", "visibility queries on",
    "off-frame recording", "a target died / changed", "render pass / framebuffer / buffer", "the classic frame after a miss (the observer looks again)", "a region clear waits for this pass (DM_VK_NATIVE_LAZYCLR)"};
enum { TLM_TARGETS, TLM_TWIN, TLM_CONFLICT, TLM_SAMPLE, TLM_END, TLM_OTHER, TLM_DRAWS, TLM_N };
static const char *const k_tlm[TLM_N] = {"other targets came", "a draw without its twin", "a foreign use of a chain texture", "a chain target sampled by a plain draw",
    "pass end (a piece / the recording's close)", "clear / area / other", "a second draw in a full-screen subpass"};
const char *vkn_tile_reason(int miss, int i) { return miss ? (i >= 0 && i < TLM_N ? k_tlm[i] : "?") : (i >= 0 && i < TLR_N ? k_tlr[i] : "?"); }

typedef struct tl_shape {
    uint8_t live, nsub, natt, prefix, streak, primed, held;
    uint32_t frame, ban_until, misses, cool;                     // cool: the frame after a miss is drawn the classic way
    tset sub[TL_SUBS];
    vkn_tex *att[TL_ATTS]; uint8_t ext[TL_ATTS];                 // ext: a read-only input that the chain does not write
    uint8_t ninput[TL_SUBS], input[TL_SUBS][VKN_MP_INPUTS];
    const vkn_shader *twin[TL_SUBS];                             // k >= 1: the twin its draw is drawn with
    uint32_t W, H;
    uint32_t prime_missing, prime_built, prime_frames;           // this frame's draws without a subpass-0 pipeline / built ahead / clean frames
} tl_shape;
static tl_shape g_tsh;                                           // the learned chain
static struct { uint8_t active, edge, cur_draws_ok; int cur; uint32_t epoch; uint64_t d0; tl_shape s; } g_ob;   // the observer's candidate of this frame
static struct { uint8_t ok, ninput; uint8_t input[VKN_MP_INPUTS]; const vkn_shader *tw; vkn_tex *ext[VKN_MP_INPUTS]; } g_tpend;   // the twin verdict of the draw being prepared
static struct {
    int st, cur; uint8_t prefix_open, entered0;
    uint32_t epoch, nsec; VkCommandBuffer sec[TL_SECS]; uint8_t sec_sub[TL_SECS];
    VkRenderPass rp; VkFramebuffer fb; uint16_t compat[TL_SUBS];
    struct { uint8_t entered, clr, dc; VkClearValue cv; } a[TL_ATTS];
    vkn_pass_info info[TL_SUBS], info_prefix;
    uint64_t d0, u0, sub_d0, sec_d0, draws; uint32_t foreign;   // draws: recorded into the chain's own buffers (the journal's count)
} g_ch;
static int g_tl_mode, g_tl_outer; static uint32_t g_tl_epoch;
int vkn_tile_hook; uint16_t vkn_tile_prime_compat; VkRenderPass vkn_tile_prime_rp;
static uint64_t g_tl_prime_ns; static uint32_t g_tl_prime_frame;
static vkn_pass_stats6 g_s6;
void vkn_pass_stats6_get(vkn_pass_stats6 *s) { if (s) { *s = g_s6; s->mode = g_tl_mode; s->streak = g_tsh.streak; s->primed = g_tsh.primed; s->nsub = g_tsh.live ? g_tsh.nsub : 0; s->natt = g_tsh.live ? g_tsh.natt : 0; } }
static uint32_t g_red_tl;
extern int vkn_lf_fused_of(const vkn_shader *fx, const vkn_shader **light, char *v) __attribute__((weak));
extern int vkn_shader_support_fill(float width, float height, void *dst, uint32_t bytes) __attribute__((weak));
extern int vkn_shlib_vs_out_prog(const struct vkn_shader *s, uint32_t location, struct nr_cv_prog *out) __attribute__((weak));
extern int vkn_shlib_named_tile(const char *fs_key, char variant, xbg_vkn_stage *out, uint64_t declared[4], vkn_tile_var *var) __attribute__((weak));
extern void vkn_draw_raw_begin(void) __attribute__((weak));
static int tl_dfr_revoke(vkn_tex *t);   // [P10 fix 6] a discarded store of the merged pass still being decided: kept (1)
static void tl_flush(int hit, int why); static void tl_end_open(int why); static void tl_rec_begin(void); static void tl_touch(vkn_tex *t, int write);
static void tl_ob_finalize(void); static VkCommandBuffer pcb(void); static void tl_verify_emit(VkCommandBuffer cb);
int vkn_tile_redraw_on;   // mode 2: the chain being recorded draws its full-screen draws twice (vkn_pass.h)
static int g_tl_redraw;   // mode 2: the draw being prepared is the classic twin of a merged draw (vkn_pass_tile.inc "verify")
// host tests: stand-ins for the shader library's facts (NULL = the library)
static int (*g_uv_fn)(const vkn_shader *s, uint32_t loc, nr_cv_prog *out); static int (*g_scale_fn)(float w, float h);
// mode 2 (verify): scratch targets, the classic twins of a chain being recorded
enum { TL_SCR = 8 };
typedef struct tl_scr { VkFormat f; uint32_t w, h; VkImage img; VkDeviceMemory mem; VkImageView view; } tl_scr;
static tl_scr g_scr[TL_SCR];
typedef struct tl_vsub { uint8_t sub, n, bad; VkCommandBuffer sec; VkRenderPass rp; VkFramebuffer fb; VkClearValue cv[VKN_PASS_COLORS]; vkn_tex *real[VKN_PASS_COLORS]; tl_scr *scr[VKN_PASS_COLORS]; uint64_t d0; } tl_vsub;
static struct { tl_vsub v[TL_SUBS]; uint32_t n, nscr; VkCommandBuffer back; vkn_pass_info info; } g_vs;
static uint32_t g_tl_vevery = 16;
