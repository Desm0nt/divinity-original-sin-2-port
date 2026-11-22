// NR-LIGHTFUSE (docs/dos2/nr/NR-LIGHTFUSE.md): DeferredSky inside ClusteredLightingWithSSAO (included by ClusteredLightingWithSSAO_mobile.frag
// with -DLIGHTFUSE; the pack's "f.ClusteredLightingWithSSAO", setup stage 8, tools/dos2_lightfuse.py). The Sky pass of the frame still begins
// (its clears of Sp and Df) but draws nothing; this shader computes Sky's two outputs per pixel and does what the Sky -> Light chain did with
// them: Sp (R11G11B10F) was read back by Light as tAmbient, Df (R11G11B10F) got Light's output added by the blender (ONE, ONE, ADD).
// DeferredSky's fast path (render scale 1 - what the host always writes, xbg_support_defaults) with the same operations and fma order as
// DeferredSky_mobile.frag (itself bit for bit the translated module). Its resources are the stash of the Sky draw (the host's spare slots):
//   bindings 200 tG0, 206 tDepth, 212 tNormal, 218 cube tIrr, 224 tBase, 230 tExposure, 236 tLut, 242 cube tSpec (textures 120..127 of the
//   fragment stage), 248 c8 (uniform slot 5 = Sky's own), 254 c6 (slot 3 = Sky's own); Sky's immediate constants c1[0].x / .y are baked
//   (SKY_C1X / SKY_C1Y = their bits, read from the player's DeferredSky bundle by the pack tool).
// q11 = the store into an unsigned 11 / 10-bit float as round to nearest even (NaN kept, <= 0 -> 0, overflow -> +inf, 11-bit denormals with
// their own step): exactly the model tools/dos2_lightfuse.py proves against. Vulkan leaves the GPU's rounding mode to the implementation,
// so on a GPU that truncates a value can differ by one step of the format (6 / 5 mantissa bits) - the documented bound.
#ifndef SKY_C1X
#define SKY_C1X 0x40638bacu   // 3.5554 (the player's bundle: 3.5553998947143555)
#endif
#ifndef SKY_C1Y
#define SKY_C1Y 0x3db851ecu   // 0.09
#endif
// [P10] the merged tile pass (docs/dos2/nr/VK-P10.md; only tools/dos2_tile_shaders.py defines OC here, the pack build never does): the
// four textures of the chain this part reads at the fragment's own pixel. OC bit 8 lfG0, 9 lfDepth, 10 lfNormal, 11 lfBase = an input
// attachment of its own (input_attachment_index = the bit); OC bit 12 = no declaration at all: the part reads the Light part's tG0 /
// tDepth / tNormal / tBase (bindings 10 / 22 / 16 / 34) - valid only when bindings 200 / 206 / 212 / 224 hold the same images as those
// (the caller's check), for devices whose input-attachment limit is below 11. Without those bits: the declarations as before.
#if (OC >> 12) & 1
#define LF_G0(p) F_G0(p)
#define LF_DEPTH(p) F_DEPTH(p)
#define LF_NORMAL(p) F_NORMAL(p)
#define LF_BASE(p) F_BASE(p)
#else
#if (OC >> 8) & 1
layout(input_attachment_index = 8, set = 0, binding = 200) uniform subpassInput lfG0;
#define LF_G0(p) subpassLoad(lfG0)
#else
layout(set = 0, binding = 200) uniform sampler2D lfG0;
#define LF_G0(p) texelFetch(lfG0, p, 0)
#endif
#if (OC >> 9) & 1
layout(input_attachment_index = 9, set = 0, binding = 206) uniform subpassInput lfDepth;
#define LF_DEPTH(p) subpassLoad(lfDepth)
#else
layout(set = 0, binding = 206) uniform sampler2D lfDepth;
#define LF_DEPTH(p) texelFetch(lfDepth, p, 0)
#endif
#if (OC >> 10) & 1
layout(input_attachment_index = 10, set = 0, binding = 212) uniform subpassInput lfNormal;
#define LF_NORMAL(p) subpassLoad(lfNormal)
#else
layout(set = 0, binding = 212) uniform sampler2D lfNormal;
#define LF_NORMAL(p) texelFetch(lfNormal, p, 0)
#endif
#endif
layout(set = 0, binding = 218) uniform samplerCube lfIrr;
#if !((OC >> 12) & 1)
#if (OC >> 11) & 1
layout(input_attachment_index = 11, set = 0, binding = 224) uniform subpassInput lfBase;
#define LF_BASE(p) subpassLoad(lfBase)
#else
layout(set = 0, binding = 224) uniform sampler2D lfBase;
#define LF_BASE(p) texelFetch(lfBase, p, 0)
#endif
#endif
layout(set = 0, binding = 230) uniform sampler2D lfExposure;
layout(set = 0, binding = 236) uniform sampler2D lfLut;
layout(set = 0, binding = 242) uniform samplerCube lfSpec;
CBUF(248, lfc8)
CBUF(254, lfc6)

vec3 lf_nanz3(vec3 v) { return mix(v, vec3(0.0), isnan(v)); }
float lf_q(float x, uint drop, float dscale, float maxv)
{
    if (isnan(x)) return x;
    if (!(x > 0.0)) return 0.0;
    if (isinf(x)) return x;
    if (x < 6.10351562e-05) return roundEven(x * dscale) / dscale;   // below 2^-14: denormals of the format, step 2^-14 / 2^mantissa
    uint u = floatBitsToUint(x);
    float r = uintBitsToFloat((u + ((1u << (drop - 1u)) - 1u) + ((u >> drop) & 1u)) & ~((1u << drop) - 1u));
    return r > maxv ? uintBitsToFloat(0x7f800000u) : r;
}
vec3 q11(vec3 v) { return vec3(lf_q(v.x, 17u, 1048576.0, 65024.0), lf_q(v.y, 17u, 1048576.0, 65024.0), lf_q(v.z, 18u, 524288.0, 64512.0)); }

// DeferredSky's outputs at this pixel (out 0 = specular IBL -> Sp, out 1 = diffuse IBL -> Df)
void lf_sky(vec4 ray, out vec3 spec, out vec3 diff)
{
    ivec2 i = ivec2(gl_FragCoord.xy);
    vec4 g0 = LF_G0(i);
    float d = LF_DEPTH(i).x;
    int model = int((uint(int(uint(max(roundEven(g0.w * 255.0), 0.0))) & 6u)) >> 1);
    spec = vec3(0.0); diff = vec3(0.0);
    [[dont_flatten]] if (d != 1.0 && model != 1) {
        vec2 n = LF_NORMAL(i).xy;
        float k = uintBitsToFloat(SKY_C1X);
        float ex = fma(n.x, k, -1.77769995), ey = fma(n.y, k, -1.77769995);
        float s = 2.0 / (fma(ey, ey, ex * ex) + 1.0);
        vec3 v = d * ray.xyz;
        float nx0 = ey * s, ny0 = ex * s, nz0 = s + -1.0;
        vec4 r0 = lfc8._m0[0], r1 = lfc8._m0[1], r2 = lfc8._m0[2];
        float Ny = fma(nz0, r1.z, fma(ny0, r1.x, nx0 * r1.y));
        float iv = -inversesqrt(fma(v.z, v.z, fma(v.y, v.y, v.x * v.x)));
        float Nx = fma(nz0, r0.z, fma(ny0, r0.x, nx0 * r0.y));
        float Nz = fma(nz0, r2.z, fma(ny0, r2.x, nx0 * r2.y));
        vec3 vn = v * iv;
        float m1 = 1.0 / max(abs(Nz), max(abs(Nx), abs(Ny)));
        float rough = max(g0.x, uintBitsToFloat(SKY_C1Y));
        float ndv = fma(vn.z, Nz, fma(vn.y, Ny, vn.x * Nx));
        float mn = -ndv;
        float Rx = fma(Nx * mn, -2.0, -vn.x), Ry = fma(Ny * mn, -2.0, -vn.y), Rz = fma(Nz * mn, -2.0, -vn.z);
        vec3 irr = texture(lfIrr, lf_nanz3(vec3(Nx * m1, Ny * m1, Nz * m1))).xyz;
        float m2 = 1.0 / max(abs(Rz), max(abs(Rx), abs(Ry)));
        vec3 base = LF_BASE(i).xyz;
        float e = texelFetch(lfExposure, ivec2(0), 0).x;
        vec3 lut = textureLod(lfLut, nanz(vec2(ndv, rough)), 0.0).xyz;
        vec4 k6 = lfc6._m0[2];
        vec3 pre = textureLod(lfSpec, lf_nanz3(vec3(Rx * m2, Ry * m2, Rz * m2)), rough * k6.y).xyz;
        float r2m = g0.y * g0.y;
        float f0 = fma(g0.z, r2m * -0.159999996, r2m * 0.159999996);
        float ce = e == 0.0 ? 0.00999999978 : e;
        float sc = k6.z;
        spec = (((pre * sc) * fma(fma(vec3(g0.z), base, vec3(f0)), vec3(lut.x), vec3(lut.y))) * 100.0) * ce;
        diff = ((fma(vec3(g0.z), -base, base) * (lut.z * (irr * sc))) * 100.0) * ce;
    }
}

#ifdef LFFOG
// NR-LIGHTFUSE fog (pack variant g, DM_NR_LIGHTFUSE_FOG): DeferredFog of the stored Df, written to out 1 (the host binds Sky's Sp there; the Fog
// draw is skipped and the readers of its target until that target's next write sample Sp). DeferredFog_mobile.frag's fast path, operation for
// operation: c = the R11G11B10F texel of Df (q11 of what out 0 stores; alpha 1), d = the same linear depth texel, Ray = the same view ray (the
// host checks: one vertex shader bundle, the same constants / streams / draw), c16[4..6] / c15[12] = Light's banks (the host checks the Fog
// draw's bytes are the same).
layout(location = 1) out vec4 outFog;
vec4 lf_fog(vec3 df, float d, vec4 ray)
{
    vec4 c = vec4(q11(df), 1.0);
    vec4 k4 = c16._m0[4], k5 = c16._m0[5], k6 = c16._m0[6];
    vec3 p = d * ray.xyz;
    float dist = sqrt(fma(p.z, p.z, fma(p.y, p.y, p.x * p.x)));
    float h = (fma(d, ray.y, c15._m0[12].y) - k6.z) * (1.0 / k6.w);
    float m = clamp(max(h, clamp((k5.w - dist) * (1.0 / (k5.w - k4.w)), 0.0, 1.0)), 0.0, 1.0);
    vec3 fy = vec3(k4.y, k5.y, k6.y), fx = vec3(k4.x, k5.x, k6.x);
    vec3 fog = fma(c.rgb - fy, vec3(m), fy);
    float t = clamp((k5.z - dist) * (1.0 / (k5.z - k4.z)), 0.0, 1.0);
    return vec4(fma(fog - fx, vec3(t), fx), c.a);
}
#define LF_OUT(v) { outColor = (v); outFog = lf_fog(outColor.rgb, d, Ray); }
#else
#define LF_OUT(v) { outColor = (v); }
#endif
