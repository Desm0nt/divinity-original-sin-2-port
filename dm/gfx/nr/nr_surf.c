// [DOS2] NR-33C DM_NR_SH_SURF (nr_surf.h): record, variant choice, the blend / depth gate of the alpha variants. Pure C.
#include "nr_surf.h"
#include <string.h>

uint64_t nr_surf_fnv64(const uint8_t *b, size_t n) {
    uint64_t x = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) x = (x ^ b[i]) * 0x100000001b3ull;
    return x;
}

const char *nr_surf_rec_parse(const uint8_t *b, size_t n, nr_surf_rec *r) {
    if (!b || !r) return "no record";
    if (n != 20) return "not 20 bytes";
    if (memcmp(b, "XSR1", 4)) return "not an XSR1 record";
    uint32_t v, z;
    memcpy(&r->spirv_fnv, b + 4, 8); memcpy(&v, b + 12, 4); memcpy(&z, b + 16, 4);
    if (!v || (v & ~0xfu) || z) return "bad variant bits";
    if (!(v & NR_SURF_V_K) && (v & (NR_SURF_V_A | NR_SURF_V_M | NR_SURF_V_P)) == 0) return "no variant";
    r->variants = v;
    return 0;
}

// Vulkan numbers (VkBlendFactor / VkBlendOp): ZERO 0, ONE 1, SRC_ALPHA 6, ONE_MINUS_SRC_ALPHA 7; ADD 0
enum { F_ONE = 1, F_SRC_ALPHA = 6, F_OMSA = 7, OP_ADD = 0 };

unsigned nr_surf_alpha_gate(const nr_surf_att *att, int n_att, uint32_t fs_out_mask, int depth_write, int stencil_write, int a2c,
                            int logic_op, int counting) {
    unsigned why = 0;
    if (n_att < 1 || !att) why |= NR_SURF_G_BLEND;
    else {
        const nr_surf_att *a = &att[0];
        // colour: src * As + dst * F = dst for As = 0 (finite src); alpha: As * G + Ad * H = Ad for As = 0 (G any finite factor)
        const int ok = !a->mask || (a->blend && a->src_c == F_SRC_ALPHA && (a->dst_c == F_OMSA || a->dst_c == F_ONE) && a->op_c == OP_ADD &&
                                    (a->dst_a == F_OMSA || a->dst_a == F_ONE) && a->op_a == OP_ADD &&
                                    // the source alpha factor must not read the destination's colour in a way that is not finite: all
                                    // VkBlendFactor values are finite numbers, so any factor gives As * G = 0
                                    a->src_a >= 0 && a->src_a <= 18);
        if (!ok) why |= NR_SURF_G_BLEND;
        for (int i = 1; i < n_att && i < 32; i++)
            if (att[i].mask && (fs_out_mask >> i & 1)) why |= NR_SURF_G_OTHER;
    }
    if (depth_write) why |= NR_SURF_G_DEPTH;
    if (stencil_write) why |= NR_SURF_G_STENCIL;
    if (a2c) why |= NR_SURF_G_A2C;
    if (logic_op) why |= NR_SURF_G_LOGIC;
    if (counting) why |= NR_SURF_G_QUERY;
    return why;
}

char nr_surf_choose(int mode, uint32_t variants, unsigned gate) {
    char alpha = 0;
    if (mode == NR_SURF_EXACT || mode == NR_SURF_CHECK) alpha = 'a';
    else if (mode == NR_SURF_MOBILE) alpha = 'm';
    else if (mode == NR_SURF_POISON) alpha = 'p';
    else return 0;
    const uint32_t bit = alpha == 'a' ? NR_SURF_V_A : alpha == 'm' ? NR_SURF_V_M : NR_SURF_V_P;
    if (!gate && (variants & bit)) return alpha;
    return (variants & NR_SURF_V_K) ? 'k' : 0;
}
