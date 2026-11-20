#version 450
#extension GL_EXT_control_flow_attributes : require
// Divinity: Original Sin 2 DE: mobile replacement of the "Light Shadows" resolve EngineShaders Shaders/NX/CascadedPCF_<N>x<S>.bshd
// (fragment stage; keys 1372db843749e1d3 = 3x768, a758e42af8853343 = 2x1024, 2a221ee245d55aab = 3x512). Built by tools/dos2_pcf_mobile.py.
//
// Interface = the translated bundle's (work/shaders-vk/<key>.xgs), word for word what the host binds:
//   binding 4  support_buffer (source 1; render-scale words, _m5[0] / _m5[1] used for the LinearDepth fetch as the translation does)
//   binding 22 fp_c15 = NVN uniform 12 PerView   (c15[12] = global_ViewPos)
//   binding 28 fp_c4  = NVN uniform 1  PGlobals2 (c4[0] LightProperties, c4[1] ShadowFarPlanes, c4[2] ShadowFrustumWidths,
//                                                 c4[23 + 4 i + r] = row r of ProjectionMatrix[i], row-major)
//   binding 34 fp_c1  = the stage's immediate constants (declared, unused: the values are compile-time constants here)
//   binding 10 LinearDepth (NVN texture 0, texelFetch), binding 16 Shadow atlas (NVN texture 1; cascade i in quadrant QUAD[i])
//   in  location 1 noperspective vec4 (TexCoord0 = view ray), out location 0 vec4 (lit fraction in all four channels)
//
// Cascade walk, fades, radius and bias are the Switch shader's (derived from the translated SPIR-V); only the filter differs:
//   PCF_MODE 0  exact:  9 point taps, manual compare (Poisson of c1[0..8]), = the Switch result, without the translation overhead
//   PCF_MODE 1  mobile: 4 textureGather taps (rotated grid), manual compare + bilinear weights = hardware 2x2 PCF, 16 texels
//   PCF_MODE 2  ipad16: 16 textureGather taps on the iPad build's Poisson disc (CascadedPCF_2x1024 iPad), 64 texels
//   PCF_MODE 3  dref:   mode 1's 4 taps as hardware depth compares (sampler2DShadow, OpImageSampleDrefExplicitLod): the sampler's
//                       LESS compare + linear filter = the same 2x2 PCF per tap, done by the texture unit (no compares / lerps in ALU)
// textureGather + manual compare needs no compare-enabled sampler (the game's NVN sampler for the atlas has none): any sampler works.
// PCF_MODE 3 needs one: the host (xbg_game.cpp, COMBINED_IMAGE_SAMPLER bind, Shader::dref_bindings) turns compare on (LESS) with a
// linear filter for a binding this module declares as a depth image (OpTypeImage Depth=1) - only binding 16 here. Differences to mode 1:
// the filter weights are the texture unit's (Vulkan: subTexelPrecisionBits, 8 on Adreno / Mali) instead of fp32 fract(), and for a UNORM
// atlas (D16 / X8_D24) the reference is clamped to [0, 1] first (a texel of exactly 0 with ref < 0 reads shadowed instead of lit).

#ifndef NCASC
#define NCASC 3
#endif
#ifndef ATLAS
#define ATLAS 1536.0            // 2 x cascade size
#endif
#ifndef PCF_MODE
#define PCF_MODE 1
#endif

layout(set = 0, binding = 4, std140) uniform support_buffer
{
    uint _m0;
    uint _m1[8];
    vec4 _m2;
    vec4 _m3;
    int _m4;
    float _m5[73];
    ivec4 _m6;
    int _m7;
} sb;
layout(set = 0, binding = 22, std140) uniform fp_c15 { vec4 _m0[4096]; } c15;
layout(set = 0, binding = 28, std140) uniform fp_c4 { vec4 _m0[4096]; } c4;
layout(set = 0, binding = 34, std140) uniform fp_c1 { vec4 _m0[4096]; } c1;
layout(set = 0, binding = 10) uniform sampler2D LinearDepth;
#if PCF_MODE == 3
layout(set = 0, binding = 16) uniform sampler2DShadow Shadow;
#else
layout(set = 0, binding = 16) uniform sampler2D Shadow;
#endif

layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

const float HALF_TEXEL = 0.5 / ATLAS;
const float RMIN = 1.0 / ATLAS, RMAX = 10.0 / ATLAS;   // tap radius clamp (cascade 0 scale), as the Switch constants
const vec2 QUAD[3] = vec2[3](vec2(0.0, 0.0), vec2(0.0, 0.5), vec2(0.5, 0.0));

struct Casc { vec3 uvz; float w; };

// One cascade's light-space position and its weight (edge fade in x / y times the far-plane fade), as the Switch shader.
Casc cascade(int i, vec3 p)
{
    const int b = 23 + 4 * i;
    vec4 r0 = c4._m0[b], r1 = c4._m0[b + 1], r2 = c4._m0[b + 2], r3 = c4._m0[b + 3];
    float x = dot(r0.xyz, p) + r0.w, y = dot(r1.xyz, p) + r1.w, z = dot(r2.xyz, p) + r2.w, w = dot(r3.xyz, p) + r3.w;
    float iw = 1.0 / w;
    vec3 uvz = vec3(x, y, z) * iw;
    vec4 lp = c4._m0[0];
    float k = 1.0 / lp.z;
    vec2 t = clamp((uvz.xy - QUAD[i]) * 2.0, 0.0, 1.0);
    vec2 fxy = clamp(k - k * abs(t * 2.0 - 1.0), 0.0, 1.0);
    float fz = clamp(clamp(1.0 - uvz.z, 0.0, 1.0) * c4._m0[1][i] * 2.0 * (1.0 / (lp.z * c4._m0[2][i])), 0.0, 1.0);
    Casc c; c.uvz = uvz; c.w = fxy.x * fxy.y * fz; return c;
}

#if PCF_MODE == 0
const vec2 TAPS[9] = vec2[9](vec2(-0.190282002, -0.129061997), vec2(0.722526014, -0.588609993), vec2(0.997680008, 0.507188022),
    vec2(-0.968993187, 0.674672008), vec2(-0.723197997, -0.978820205), vec2(0.0459919982, 0.947386026),
    vec2(-0.977172196, -0.220313996), vec2(0.0448319986, -0.969542623), vec2(-0.57130599, 0.50633198));
#define NTAPS 9
#elif PCF_MODE == 1 || PCF_MODE == 3
const vec2 TAPS[4] = vec2[4](vec2(-0.25, -0.75), vec2(0.75, -0.25), vec2(0.25, 0.75), vec2(-0.75, 0.25));
#define NTAPS 4
#else
const vec2 TAPS[16] = vec2[16](vec2(-0.641573071, -0.170358405), vec2(-0.220173702, 0.0666730404), vec2(-0.565267920, 0.409814090),
    vec2(-0.372753888, -0.690129280), vec2(-0.963876784, 0.0795568228), vec2(0.0895406604, -0.433672994),
    vec2(-0.815210223, -0.533595324), vec2(-0.0262216199, 0.725986183), vec2(0.152993307, 0.216891706),
    vec2(-0.394465297, 0.896840572), vec2(0.566493392, 0.677397907), vec2(0.655839980, -0.0949900225),
    vec2(0.0658113286, -0.885984778), vec2(0.465619504, -0.590899110), vec2(0.771781623, 0.291234612),
    vec2(0.862963498, -0.501030385));
#define NTAPS 16
#endif

// Lit fraction (0..1) of cascade i around uvz: Switch radius / bias, clamped to the cascade's quadrant.
float pcf(int i, vec3 uvz)
{
    vec4 lp = c4._m0[0];
    float s = c4._m0[2][i] / c4._m0[2][0];                       // frustum width relative to cascade 0 (1 for cascade 0)
    float r = max(min(lp.x * 3.5 / c4._m0[2][i], RMAX / s), RMIN / s);
    float ref = uvz.z - s * (lp.w / c4._m0[1][i]);
    float sum = 0.0;
#if PCF_MODE == 0
    vec2 lo = QUAD[i] + HALF_TEXEL, hi = QUAD[i] + 0.5 - HALF_TEXEL;
    [[unroll]] for (int j = 0; j < NTAPS; j++) {
        vec2 uv = clamp(uvz.xy + r * TAPS[j], lo, hi);
        sum += float(ref < textureLod(Shadow, uv, 0.0).x);
    }
#elif PCF_MODE == 3
    vec2 lo = QUAD[i] + 2.0 * HALF_TEXEL, hi = QUAD[i] + 0.5 - 2.0 * HALF_TEXEL;   // as mode 1: the 2x2 footprint stays inside the quadrant
    [[unroll]] for (int j = 0; j < NTAPS; j++)
        sum += textureLod(Shadow, vec3(clamp(uvz.xy + r * TAPS[j], lo, hi), ref), 0.0);   // 1 = ref < texel (LESS), bilinear over 2x2
#else
    vec2 lo = QUAD[i] + 2.0 * HALF_TEXEL, hi = QUAD[i] + 0.5 - 2.0 * HALF_TEXEL;   // the 2x2 footprint stays inside the quadrant
    [[unroll]] for (int j = 0; j < NTAPS; j++) {
        vec2 uv = clamp(uvz.xy + r * TAPS[j], lo, hi);
        vec4 d = textureGather(Shadow, uv, 0);                  // x (0,1) y (1,1) z (1,0) w (0,0)
        vec4 l = vec4(lessThan(vec4(ref), d));
        vec2 f = fract(uv * ATLAS - 0.5);
        vec2 row = mix(l.wx, l.zy, f.x);                         // (j0 row, j1 row)
        sum += mix(row.x, row.y, f.y);
    }
#endif
    return sum * (1.0 / float(NTAPS));
}

// One cascade of the Switch walk: the first cascade with a weight > 0 decides; inside its fade band the next cascade is blended
// in with (1 - w) (the next cascade's own weight only gates it), at its outer edge with no next cascade the result fades to w * lit.
void cascadeStep(const int i, vec3 p, inout float result, inout bool done)
{
    if (done) return;
    Casc c = cascade(i, p);
    if (!(c.w > 0.0)) return;
    done = true;
    float a = c.w * pcf(i, c.uvz);
    if (i + 1 < NCASC && c.w < 1.0) {
        Casc n = cascade(min(i + 1, NCASC - 1), p);
        result = n.w > 0.0 ? a + (1.0 - c.w) * pcf(min(i + 1, NCASC - 1), n.uvz) : a;
    } else {
        result = a - c.w + 1.0;
    }
}

void main()
{
    // LinearDepth texel of this pixel, with the translation's render-scale handling (support buffer words 1 / 2 of _m5)
    float rs = sb._m5[0], sc = sb._m5[1];
    ivec2 base = ivec2(trunc(gl_FragCoord.xy / rs));
    ivec2 px;
    if (sc == 1.0) px = base;
    else if (sc < 0.0) px = ivec2(fma(vec2(base), vec2(-sc), mod(gl_FragCoord.xy, vec2(-sc))));
    else px = ivec2(vec2(base) * sc);
    float depth = texelFetch(LinearDepth, px, 0).x;
    vec3 p = depth * TexCoord0.xyz + c15._m0[12].xyz;

    // Unrolled by hand: the cascade index is a constant in every copy (no dynamic indexing of QUAD / vec4 components).
    float result = 1.0;
    bool done = false;
    cascadeStep(0, p, result, done);
    cascadeStep(1, p, result, done);
#if NCASC > 2
    cascadeStep(2, p, result, done);
#endif
    outColor = vec4(max(0.0, result));
}
