// [DOS2] NR-04: engine shaders by name (DM_NR_SH, default 0; docs/dos2/nr/NR-04.md). Pure C: no Vulkan, no game addresses, no host types.
// The table below names the EngineShaders programs whose fragment stage has an own GLSL rewrite (tools/dos2_post, tools/dos2_pcf), what
// class it belongs to and which variants exist. The bundles themselves are NOT in the APK: setup (stage 8, tools/dos2_nr_sh_pack.py) builds
// <shaders>/native-nr.xgspack on the player's machine from the player's own translated bundles (head byte for byte) + the rewrites compiled
// with glslc, keyed "<variant>.<engine name>" ("x.FadeBlending", "m.CascadedPCF_3x768"). The host (xbg_game.cpp namespace nrsh) looks the
// translated pack key up here, reads the named bundle and checks it (nr_sh_head_equal, nr_sh_io, nr_sh_bindings_ok) before it swaps the
// SPIR-V; at pipeline creation nr_sh_pass_check holds the pass the shader is drawn into against the pass it was verified for.
// The key decides (it is the code); with DM_NR=1/2 the registry hook of CreateShader reports every engine shader's name, path and fragment
// key (nr_sh_name_note): agreement is counted, a name the table does not know is reported, and only our key under the name of another
// entry (two codes swapped) turns that entry off (the translated shader is used from then on).
// Thread safe: the table is constant; the per-entry state is atomic.
#ifndef DM_NR_SH_H
#define DM_NR_SH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NR_SH_POST = 0, NR_SH_LIGHT = 1, NR_SH_SHADOW = 2, NR_SH_CLASSES = 3 };
// DM_NR_SH_PCF (the shadow class): 0 = translated (default), 1 = exact (9 point taps, the Switch result), 2 = mobile (4 textureGather, a
// smoother 2x2 PCF edge - another look, needs the owner's screenshot A/B), 3 = dref (PERF-X19 T11: mobile's 4 taps as hardware depth
// compares, sampler2DShadow; the host turns compare LESS + linear filtering on for the atlas binding; pack variant 'd'; a device whose
// atlas format cannot filter linearly / compare takes 2)
enum { NR_SH_PCF_OFF = 0, NR_SH_PCF_EXACT = 1, NR_SH_PCF_MOBILE = 2, NR_SH_PCF_DREF = 3 };

typedef struct nr_sh_entry {
    const char *name;        // engine program name (EngineShaders:Shaders/NX/<name>.bshd)
    const char *key;         // xbg_shader_program_key of its fragment stage = the translated bundle's name
    uint8_t cls;             // NR_SH_POST / LIGHT / SHADOW
    const char *variants;    // pack variants: 'x' exact (bit for bit where proven), 'm' mobile
    uint8_t proven;          // 1: tools/spv_interp.py ran translated and rewrite on the same random inputs, outputs equal bit for bit
    uint8_t colors;          // the pass it was verified in (phone logs gp1 / gp2, 16.11): colour attachments
    uint16_t color0;         //   and the NVN format of colour attachment 0 (0x1 R8, 0x6 R16, 0x25 RGBA8, 0x3f R11G11B10F)
} nr_sh_entry;

extern const nr_sh_entry nr_sh_table[];
extern const int nr_sh_count;

int nr_sh_find_key(const char *key);           // 16 hex characters (a longer string: its first 16) -> table index, -1 none
int nr_sh_find_name(const char *name);         // "CascadedPCF_3x768", "rom:/Data/Shaders/NX/CascadedPCF_3x768.bshd", any case -> index, -1;
                                               // path-qualified: a name with a folder other than Shaders/NX is -1 (another shader)
// The variant the host takes for entry i (0 = keep the translated one): post / light 'x'; shadows by pcf (NR_SH_PCF_*).
char nr_sh_variant(int i, int pcf);
// Pack key "<v>.<name>" (xgs_pack.py: at most 31 characters); 0 = too long / bad index.
int nr_sh_pack_key(int i, char v, char *out, size_t n);

// ---- bundles (the host's xbg_shader_header: 8 u32 magic 'XGS1', version 1, stage, bindings, inputs, c1 bytes, SPIR-V bytes, reserved;
// then 32-byte bindings, 4-byte inputs, c1, SPIR-V) ----
size_t nr_sh_head(const uint8_t *b, size_t n);              // bytes before the SPIR-V; 0 = not a well-formed bundle
// 1: both well formed and their heads equal byte for byte except header word 6 (the SPIR-V length, which is the rewrite's own)
int nr_sh_head_equal(const uint8_t *a, size_t na, const uint8_t *b, size_t nb);
uint64_t nr_sh_head_hash(const uint8_t *b, size_t n);       // FNV-1a 64 of the head without word 6 (log lines); 0 = bad bundle
// Location masks of the entry point's Input / Output variables (no built-ins); 0 = a well-formed module (else -1)
int nr_sh_io(const uint32_t *w, size_t n, uint32_t *in, uint32_t *out);
// 1: every descriptor the SPIR-V declares (set 0, binding N) is in the bundle's binding table, so the host's layout binds it
int nr_sh_bindings_ok(const uint8_t *bundle, size_t n);

// T11 (Dref): bit b set = descriptor set 0, binding b (< 64) is a UniformConstant variable whose (sampled) image type has Depth = 1, i.e. the
// module samples it with OpImage*Dref* (glslc: sampler2DShadow). The host gives exactly these bindings a compare sampler. 0 = none / bad module.
uint64_t nr_sh_depth_bindings(const uint32_t *w, size_t n);

// ---- engine names (registry cross-check) ----
// AGREE: the table's name for this key is the Get name or the Load path (nr_sh_find_name: in Shaders/NX, any case, without .bshd; a
// shader of the same base name in another folder, e.g. Shaders/NX/Orthographic/LinearizeDepth.bshd, is not the table's); OTHER: not ours / nothing
// to compare; KEY_DIFFERS: a name of the table with another fragment key (another code under our name - our key is untouched, reported);
// NAME_DIFFERS: our key under a name the table does not know (reported); SWAPPED: our key under the name of another entry - that entry's
// key is turned off (nr_sh_enabled 0). *idx = the entry concerned (-1 none).
enum { NR_SH_NAME_OTHER = 0, NR_SH_NAME_AGREE = 1, NR_SH_NAME_KEY_DIFFERS = -1, NR_SH_NAME_NAME_DIFFERS = -2, NR_SH_NAME_SWAPPED = -3 };
int nr_sh_name_note(const char *name, const char *path, const char *fs_key, int *idx);
int nr_sh_enabled(int i);
void nr_sh_disable(int i);
uint64_t nr_sh_names_agreed(void);

// ---- the pass the shader is drawn into (pipeline creation) ----
// Signature: active colour attachments in slot order (NVN formats), depth (NVN format, 0 = none), samples -> FNV-1a 32 ("pass hash").
uint32_t nr_sh_pass_hash(int ncolor, const uint32_t *fmt, uint32_t depth_fmt, uint32_t samples);
enum { NR_SH_P_COLORS = 1, NR_SH_P_FORMAT = 2, NR_SH_P_SAMPLES = 4 };
// 0 = the verified pass (colour count, format of colour 0, one sample); else NR_SH_P_* bits: not verified there.
unsigned nr_sh_pass_check(int i, int ncolor, const uint32_t *fmt, uint32_t samples);

#ifdef __cplusplus
}
#endif
#endif
