// [DOS2] NR-04: engine shaders by name (DM_NR_SH). See nr_sh.h; docs/dos2/nr/NR-04.md.
#include "nr_sh.h"
#include <ctype.h>
#include <string.h>

// Keep in step with tools/dos2_nr_sh_pack.py ENTRIES (source_structure_test.py compares the two). Keys: work/shaders-jobs/programs.tsv
// (EngineShaders), fragment stage. The verified pass: NVN GPASS lines of gp1 / gp2 (Mi 14 Ultra, 3D 2160x972): one draw, one colour.
const nr_sh_entry nr_sh_table[] = {
    {"FadeBlending",              "62d683cc49953ad6", NR_SH_POST,   "x",  1, 1, 0x3f},
    {"EoCFog",                    "e94ea226e689801c", NR_SH_POST,   "x",  1, 1, 0x25},
    {"EoCOutlineCombine",         "010472595b342188", NR_SH_POST,   "x",  1, 1, 0x25},
    {"LinearizeDepth",            "f23c09d198b55dc5", NR_SH_POST,   "x",  1, 1, 0x06},
    {"PPDownSampleLog2",          "cbe8649300e9c8cd", NR_SH_POST,   "x",  1, 1, 0x3f},
    {"DeferredSky",               "640a4c497bcc748b", NR_SH_LIGHT,  "x",  1, 2, 0x3f},
    {"ClusteredLightingWithSSAO", "d236b8d5b7ffad3c", NR_SH_LIGHT,  "x",  1, 1, 0x3f},
    {"CascadedPCF_3x768",         "1372db843749e1d3", NR_SH_SHADOW, "xmd", 0, 1, 0x01},
    {"CascadedPCF_2x1024",        "a758e42af8853343", NR_SH_SHADOW, "xmd", 0, 1, 0x01},
    {"CascadedPCF_3x512",         "2a221ee245d55aab", NR_SH_SHADOW, "xmd", 0, 1, 0x01},
    /* NR-33A (engine post) */
    {"DeferredFog",               "d8aeec640d465275", NR_SH_POST,   "x",  1, 1, 0x3f},
    {"PPBloomUpsampleAndBlur",    "4e290c9cf1162fe9", NR_SH_POST,   "x",  1, 1, 0x3f},
    {"CombineUI_NoFetch",         "98186c22a7209861", NR_SH_POST,   "x",  1, 1, 0x25},
    {"PPBloomScaleBiasDownsampleAndBlur", "74fe9ae2066d53da", NR_SH_POST, "x", 1, 1, 0x3f},
    {"PPBloomDownsampleAndBlur",  "0e3fc07b320db1e9", NR_SH_POST,   "x",  1, 1, 0x3f},
    {"PPFXAA",                    "691768aa08545d95", NR_SH_POST,   "x",  1, 1, 0x25},
    /* end NR-33A (engine post) */
};
const int nr_sh_count = (int)(sizeof nr_sh_table / sizeof nr_sh_table[0]);

#define NR_SH_MAX 16
static int g_off[NR_SH_MAX];      // 1 = disabled by a name contradiction
static uint64_t g_agreed;

int nr_sh_find_key(const char *key) {
    if (!key) return -1;
    for (int i = 0; i < nr_sh_count; i++) if (!strncmp(nr_sh_table[i].key, key, 16)) return i;
    return -1;
}

// "Name", "Name.bshd", "Shaders/NX/Name.bshd", "rom:/Data/Shaders/NX/Name.bshd", "EngineShaders:Shaders/NX/Name.bshd" -> compares the base
// name without .bshd, any case. Path-qualified: every entry is an EngineShaders program directly in Shaders/NX, so a name with a folder is
// the table's only when that folder is Shaders/NX (any case, '/' or '\'; what precedes the last ':' - a pak or a drive - is ignored). The
// engine has other shaders of the same base name in subfolders (Shaders/NX/Orthographic/LinearizeDepth.bshd: another fragment key, a2cc..
// against f23c..); they are other shaders, not the table's name with another code.
static int nr_sh_sep(char c) { return c == '/' || c == '\\'; }
static int nr_sh_dir_is_nx(const char *s, size_t n) {   // s[0..n) is "Shaders/NX" or ends in "<sep>Shaders/NX"
    static const char nx[] = "shaders/nx";
    const size_t m = sizeof nx - 1;
    if (n < m || (n > m && !nr_sh_sep(s[n - m - 1]))) return 0;
    for (size_t k = 0; k < m; k++) {
        const char c = s[n - m + k];
        if (nx[k] == '/' ? !nr_sh_sep(c) : tolower((unsigned char)c) != nx[k]) return 0;
    }
    return 1;
}
int nr_sh_find_name(const char *name) {
    if (!name) return -1;
    const char *s = name, *b = NULL;
    for (const char *p = name; *p; p++) if (*p == ':') s = p + 1;
    for (const char *p = s; *p; p++) if (nr_sh_sep(*p)) b = p + 1;
    if (!b) b = s;
    else if (!nr_sh_dir_is_nx(s, (size_t)(b - 1 - s))) return -1;   // another folder: another shader of this base name
    size_t n = strlen(b);
    if (n > 5 && !strncmp(b + n - 5, ".bshd", 5)) n -= 5;
    for (int i = 0; i < nr_sh_count; i++) {
        const char *t = nr_sh_table[i].name;
        if (strlen(t) != n) continue;
        size_t k = 0;
        while (k < n && tolower((unsigned char)t[k]) == tolower((unsigned char)b[k])) k++;
        if (k == n) return i;
    }
    return -1;
}

char nr_sh_variant(int i, int pcf) {
    if (i < 0 || i >= nr_sh_count) return 0;
    const nr_sh_entry *e = &nr_sh_table[i];
    if (e->cls != NR_SH_SHADOW) return strchr(e->variants, 'x') ? 'x' : 0;
    const char v = pcf == NR_SH_PCF_EXACT ? 'x' : pcf == NR_SH_PCF_MOBILE ? 'm' : pcf == NR_SH_PCF_DREF ? 'd' : 0;
    return v && strchr(e->variants, v) ? v : 0;
}

int nr_sh_pack_key(int i, char v, char *out, size_t n) {
    if (i < 0 || i >= nr_sh_count || !v || !out || n < 32) return 0;
    const char *s = nr_sh_table[i].name;   /* NR-33A: a name too long for an xgspack key (31 characters) -> "<v>.<fragment key>" */
    if (strlen(s) + 2 > 31) s = nr_sh_table[i].key;
    const size_t l = strlen(s);
    out[0] = v; out[1] = '.'; memcpy(out + 2, s, l + 1);
    return 1;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

size_t nr_sh_head(const uint8_t *b, size_t n) {
    if (!b || n < 32) return 0;
    uint32_t h[8];
    for (int i = 0; i < 8; i++) h[i] = rd32(b + 4 * i);
    if (h[0] != 0x31534758u || h[1] != 1 || h[3] > 256 || h[4] > 16 || h[5] > 65536 || h[6] % 4) return 0;
    const size_t head = 32 + (size_t)h[3] * 32 + (size_t)h[4] * 4 + h[5];
    return head + h[6] == n ? head : 0;
}

int nr_sh_head_equal(const uint8_t *a, size_t na, const uint8_t *b, size_t nb) {
    const size_t ha = nr_sh_head(a, na), hb = nr_sh_head(b, nb);
    return ha && ha == hb && !memcmp(a, b, 24) && !memcmp(a + 28, b + 28, ha - 28);
}

uint64_t nr_sh_head_hash(const uint8_t *b, size_t n) {
    const size_t h = nr_sh_head(b, n);
    if (!h) return 0;
    uint64_t x = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < h; i++) { if (i >= 24 && i < 28) continue; x ^= b[i]; x *= 0x100000001b3ull; }
    return x;
}

int nr_sh_io(const uint32_t *w, size_t n, uint32_t *in, uint32_t *out) {
    *in = *out = 0;
    if (!w || n < 5 || w[0] != 0x07230203u) return -1;
    enum { MAXV = 512 };
    uint32_t iface[64]; int ni = 0;
    uint32_t lid[MAXV], lval[MAXV]; int nl = 0;
    uint32_t vid[MAXV], vsc[MAXV]; int nv = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t c = w[i] >> 16, op = w[i] & 0xffff;
        if (!c || i + c > n) return -1;
        if (op == 15 && c > 3) {   // OpEntryPoint: model, id, name (nul-terminated, word padded), interface ids
            size_t k = i + 3;
            while (k < i + c && (w[k] & 0xff000000u) && (w[k] & 0xff0000u) && (w[k] & 0xff00u) && (w[k] & 0xffu)) k++;
            for (k++; k < i + c && ni < 64; k++) iface[ni++] = w[k];
        } else if (op == 71 && c >= 4 && w[i + 2] == 30 && nl < MAXV) { lid[nl] = w[i + 1]; lval[nl++] = w[i + 3]; }   // Location
        else if (op == 59 && c >= 4 && nv < MAXV) { vid[nv] = w[i + 2]; vsc[nv++] = w[i + 3]; }                          // OpVariable
        i += c;
    }
    for (int k = 0; k < ni; k++) {
        int l = -1, s = -1;
        for (int j = 0; j < nl; j++) if (lid[j] == iface[k]) { l = (int)lval[j]; break; }
        for (int j = 0; j < nv; j++) if (vid[j] == iface[k]) { s = (int)vsc[j]; break; }
        if (l < 0 || l >= 32 || s < 0) continue;
        if (s == 1) *in |= 1u << l; else if (s == 3) *out |= 1u << l;
    }
    return 0;
}

int nr_sh_bindings_ok(const uint8_t *b, size_t n) {
    const size_t head = nr_sh_head(b, n);
    if (!head) return 0;
    const uint32_t nb = rd32(b + 12);
    const uint8_t *s = b + head;   // read word by word: c1 bytes need not keep the SPIR-V 4-byte aligned
    const size_t nw = (n - head) / 4;
    if (nw < 5 || rd32(s) != 0x07230203u) return 0;
    for (size_t i = 5; i < nw;) {
        const uint32_t w0 = rd32(s + 4 * i), c = w0 >> 16, op = w0 & 0xffff;
        if (!c || i + c > nw) return 0;
        if (op == 71 && c >= 4) {
            const uint32_t dec = rd32(s + 4 * (i + 2)), val = rd32(s + 4 * (i + 3));
            if (dec == 34 && val != 0) return 0;   // DescriptorSet other than 0
            if (dec == 33) {                       // Binding
                int found = 0;
                for (uint32_t k = 0; k < nb && !found; k++) found = rd32(b + 32 + 32 * (size_t)k) == val;
                if (!found) return 0;
            }
        }
        i += c;
    }
    return 1;
}

uint64_t nr_sh_depth_bindings(const uint32_t *w, size_t n) {
    if (!w || n < 5 || w[0] != 0x07230203u) return 0;
    enum { MAXI = 64 };
    uint32_t dimg[MAXI]; int nd = 0;      // OpTypeImage ids with Depth = 1, then OpTypeSampledImage of them
    uint32_t ptr[MAXI]; int np = 0;       // OpTypePointer UniformConstant -> one of those
    uint32_t vid[MAXI]; int nv = 0;       // OpVariable of such a pointer
    uint32_t bid[256], bval[256], sid[256], sval[256]; int nb = 0, ns = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t c = w[i] >> 16, op = w[i] & 0xffff;
        if (!c || i + c > n) return 0;
        int k;
        if (op == 25 && c >= 9 && w[i + 4] == 1 && nd < MAXI) dimg[nd++] = w[i + 1];                     // OpTypeImage ... Depth 1
        else if (op == 27 && c >= 3) { for (k = 0; k < nd && dimg[k] != w[i + 2]; k++) {} if (k < nd && nd < MAXI) dimg[nd++] = w[i + 1]; }
        else if (op == 32 && c >= 4 && w[i + 2] == 0) { for (k = 0; k < nd && dimg[k] != w[i + 3]; k++) {} if (k < nd && np < MAXI) ptr[np++] = w[i + 1]; }
        else if (op == 59 && c >= 4 && w[i + 3] == 0) { for (k = 0; k < np && ptr[k] != w[i + 1]; k++) {} if (k < np && nv < MAXI) vid[nv++] = w[i + 2]; }
        else if (op == 71 && c >= 4 && w[i + 2] == 33 && nb < 256) { bid[nb] = w[i + 1]; bval[nb++] = w[i + 3]; }   // Binding
        else if (op == 71 && c >= 4 && w[i + 2] == 34 && ns < 256) { sid[ns] = w[i + 1]; sval[ns++] = w[i + 3]; }   // DescriptorSet
        i += c;
    }
    uint64_t m = 0;
    for (int v = 0; v < nv; v++) {
        int b = -1, set = 0;
        for (int k = 0; k < nb; k++) if (bid[k] == vid[v]) { b = (int)bval[k]; break; }
        for (int k = 0; k < ns; k++) if (sid[k] == vid[v]) { set = (int)sval[k]; break; }
        if (b >= 0 && b < 64 && set == 0) m |= 1ull << b;
    }
    return m;
}

int nr_sh_enabled(int i) { return i >= 0 && i < nr_sh_count && i < NR_SH_MAX && !__atomic_load_n(&g_off[i], __ATOMIC_RELAXED); }
void nr_sh_disable(int i) { if (i >= 0 && i < nr_sh_count && i < NR_SH_MAX) __atomic_store_n(&g_off[i], 1, __ATOMIC_RELAXED); }
uint64_t nr_sh_names_agreed(void) { return __atomic_load_n(&g_agreed, __ATOMIC_RELAXED); }

// The key decides (it is the code: SHA-256 of control + code); the name is a cross-check. Only a name of another entry of the table on our
// key (two engine shaders' codes swapped) turns the entry off; a name the table does not know is reported, not acted on.
int nr_sh_name_note(const char *name, const char *path, const char *fs_key, int *idx) {
    const int bn = name && *name ? nr_sh_find_name(name) : -1, bp = path && *path ? nr_sh_find_name(path) : -1;
    const int bk = fs_key && *fs_key ? nr_sh_find_key(fs_key) : -1;
    if (idx) *idx = bk >= 0 ? bk : bn >= 0 ? bn : bp;
    if (bk < 0) return (bn >= 0 || bp >= 0) && fs_key && *fs_key ? NR_SH_NAME_KEY_DIFFERS : NR_SH_NAME_OTHER;
    if (bn == bk || bp == bk) { __atomic_add_fetch(&g_agreed, 1, __ATOMIC_RELAXED); return NR_SH_NAME_AGREE; }
    if (bn >= 0 || bp >= 0) { nr_sh_disable(bk); return NR_SH_NAME_SWAPPED; }
    if ((!name || !*name) && (!path || !*path)) return NR_SH_NAME_OTHER;
    return NR_SH_NAME_NAME_DIFFERS;
}

uint32_t nr_sh_pass_hash(int ncolor, const uint32_t *fmt, uint32_t depth_fmt, uint32_t samples) {
    uint32_t x = 2166136261u;
#define NR_SH_MIX(v) do { uint32_t m_ = (v); for (int b_ = 0; b_ < 4; b_++) { x ^= (m_ >> (8 * b_)) & 0xff; x *= 16777619u; } } while (0)
    NR_SH_MIX((uint32_t)ncolor);
    for (int i = 0; i < ncolor && i < 8; i++) NR_SH_MIX(fmt ? fmt[i] : 0);
    NR_SH_MIX(depth_fmt); NR_SH_MIX(samples);
#undef NR_SH_MIX
    return x;
}

unsigned nr_sh_pass_check(int i, int ncolor, const uint32_t *fmt, uint32_t samples) {
    if (i < 0 || i >= nr_sh_count) return NR_SH_P_COLORS | NR_SH_P_FORMAT | NR_SH_P_SAMPLES;
    const nr_sh_entry *e = &nr_sh_table[i];
    unsigned why = 0;
    if (ncolor != e->colors) why |= NR_SH_P_COLORS;
    if (ncolor < 1 || !fmt || fmt[0] != e->color0) why |= NR_SH_P_FORMAT;
    if (samples != 1) why |= NR_SH_P_SAMPLES;
    return why;
}
