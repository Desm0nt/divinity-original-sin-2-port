#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 DeferredSky (EngineShaders:Shaders/NX/DeferredSky.bshd, fragment key 640a4c497bcc748b): full frame, image-based ambient of the
// lit G-buffer pixels, two MRT (out 0 = specular IBL, out 1 = diffuse IBL; both read back by ClusteredLightingWithSSAO / the next pass).
// Derived from the translated SPIR-V (spirv-opt -O):
//   g0 = t_E (x roughness, y reflectance, z metal mask, w flags * 255), d = t_8.x (linear depth), n = t_A.xy (stereographic normal,
//   fma(n, c1[0].x, -1.7777)), base = t_C.rgb, e = t_32(0, 0).x (exposure, 0 -> 0.01).
//   Sky (d == 1, unordered) or shading model 1 (unlit) -> both outputs 0. Else N = c8 rows * decoded view normal, V = -normalize(d * ray),
//   R = reflect; diffuse = (1 - metal) * base * lut.z * irradiance(N) * c6[2].z * 100 * e; specular = prefiltered(R, lod rough *
//   c6[2].y) * c6[2].z * fma(fma(metal, base, F0), lut.x, lut.y) * 100 * e, F0 = 0.16 * refl^2 * (1 - metal) (as fma), lut = t_30(ndv, rough).
// Same operations and fma order as the translated module, so the result is the same float for float.
// Interface = the translated bundle's: 4 support, 58 fp_c1, 64 fp_c8, 70 fp_c6, 10 t_E, 16 t_8, 22 t_A, 28 cube t_28, 34 t_C, 40 t_32,
// 46 t_30, 52 cube t_2C; in location 2 noperspective (view ray); out 0, 1.
// Fast path (uniform branch): render scales _m5[0..3,5,6] all 1 (what the host writes): no per-fetch scale fix-up (12 of them in the
// translation); sky / unlit pixels stop after 3 fetches (the translation always fetches t_E, t_8, t_A).
#define NEED_RS
#include "common.glsl"
SUPPORT_BUFFER(4)
CBUF(58, c1)
CBUF(64, c8)
CBUF(70, c6)
// NR-ONCHIP (docs/dos2/nr/NR-ONCHIP.md): with OC != 0 (only tools/dos2_onchip_shaders.py defines it) texture n below with bit n of OC set is
// an input attachment (input_attachment_index n) of a merged render pass: 0 tG0, 1 tDepth, 2 tNormal, 3 tBase; render scale 1 only.
#ifndef OC
#define OC 0
#endif
#if (OC >> 0) & 1
layout(input_attachment_index = 0, set = 0, binding = 10) uniform subpassInput tG0;
#define F_G0(p) subpassLoad(tG0)
#else
layout(set = 0, binding = 10) uniform sampler2D tG0;
#define F_G0(p) texelFetch(tG0, p, 0)
#endif
#if (OC >> 1) & 1
layout(input_attachment_index = 1, set = 0, binding = 16) uniform subpassInput tDepth;
#define F_DEPTH(p) subpassLoad(tDepth)
#else
layout(set = 0, binding = 16) uniform sampler2D tDepth;
#define F_DEPTH(p) texelFetch(tDepth, p, 0)
#endif
#if (OC >> 2) & 1
layout(input_attachment_index = 2, set = 0, binding = 22) uniform subpassInput tNormal;
#define F_NORMAL(p) subpassLoad(tNormal)
#else
layout(set = 0, binding = 22) uniform sampler2D tNormal;
#define F_NORMAL(p) texelFetch(tNormal, p, 0)
#endif
layout(set = 0, binding = 28) uniform samplerCube tIrr;
#if (OC >> 3) & 1
layout(input_attachment_index = 3, set = 0, binding = 34) uniform subpassInput tBase;
#define F_BASE(p) subpassLoad(tBase)
#else
layout(set = 0, binding = 34) uniform sampler2D tBase;
#define F_BASE(p) texelFetch(tBase, p, 0)
#endif
layout(set = 0, binding = 40) uniform sampler2D tExposure;
layout(set = 0, binding = 46) uniform sampler2D tLut;
layout(set = 0, binding = 52) uniform samplerCube tSpec;
layout(location = 2) noperspective in vec4 Ray;
layout(location = 0) out vec4 outSpec;
layout(location = 1) out vec4 outDiff;

vec3 nanz3(vec3 v) { return mix(v, vec3(0.0), isnan(v)); }

void main()
{
    ivec2 pE, pD, pN, pB, pX;
#if OC
    {   // render scale 1 only
#else
    [[dont_flatten]] if (sb._m5[0] == 1.0 && sb._m5[1] == 1.0 && sb._m5[2] == 1.0 && sb._m5[3] == 1.0 && sb._m5[5] == 1.0 &&
                         sb._m5[6] == 1.0) {
#endif
        ivec2 i = ivec2(gl_FragCoord.xy);
        pE = i; pD = i; pN = i; pB = i; pX = ivec2(0);
    }
#if !OC
    else {
        float s0 = sb._m5[0];
        ivec2 c = ivec2(int(trunc(gl_FragCoord.x / s0)), int(trunc(gl_FragCoord.y / s0)));
        pE = rs_fix2(c, sb._m5[1]); pD = rs_fix2(c, sb._m5[2]); pN = rs_fix2(c, sb._m5[3]); pB = rs_fix2(c, sb._m5[5]);
        float s6 = sb._m5[6];
        pX = s6 < 0.0 ? ivec2(int(mod(gl_FragCoord.x, -s6)), int(mod(gl_FragCoord.y, -s6))) : ivec2(0);
    }
#endif
    vec4 g0 = F_G0(pE);
    float d = F_DEPTH(pD).x;
    int model = int((uint(int(uint(max(roundEven(g0.w * 255.0), 0.0))) & 6u)) >> 1);
    vec3 spec = vec3(0.0), diff = vec3(0.0);
    [[dont_flatten]] if (d != 1.0 && model != 1) {
        vec2 n = F_NORMAL(pN).xy;
        float k = c1._m0[0].x;
        float ex = fma(n.x, k, -1.77769995), ey = fma(n.y, k, -1.77769995);
        float s = 2.0 / (fma(ey, ey, ex * ex) + 1.0);
        vec3 v = d * Ray.xyz;
        float nx0 = ey * s, ny0 = ex * s, nz0 = s + -1.0;
        vec4 r0 = c8._m0[0], r1 = c8._m0[1], r2 = c8._m0[2];
        float Ny = fma(nz0, r1.z, fma(ny0, r1.x, nx0 * r1.y));
        float iv = -inversesqrt(fma(v.z, v.z, fma(v.y, v.y, v.x * v.x)));
        float Nx = fma(nz0, r0.z, fma(ny0, r0.x, nx0 * r0.y));
        float Nz = fma(nz0, r2.z, fma(ny0, r2.x, nx0 * r2.y));
        vec3 vn = v * iv;
        float m1 = 1.0 / max(abs(Nz), max(abs(Nx), abs(Ny)));
        vec4 k1 = c1._m0[0];
        float rough = max(g0.x, k1.y);
        float ndv = fma(vn.z, Nz, fma(vn.y, Ny, vn.x * Nx));
        float mn = -ndv;
        float Rx = fma(Nx * mn, -2.0, -vn.x), Ry = fma(Ny * mn, -2.0, -vn.y), Rz = fma(Nz * mn, -2.0, -vn.z);
        vec3 irr = texture(tIrr, nanz3(vec3(Nx * m1, Ny * m1, Nz * m1))).xyz;
        float m2 = 1.0 / max(abs(Rz), max(abs(Rx), abs(Ry)));
        vec3 base = F_BASE(pB).xyz;
        float e = texelFetch(tExposure, pX, 0).x;
        vec3 lut = textureLod(tLut, nanz(vec2(ndv, rough)), 0.0).xyz;
        vec4 k6 = c6._m0[2];
        vec3 pre = textureLod(tSpec, nanz3(vec3(Rx * m2, Ry * m2, Rz * m2)), rough * k6.y).xyz;
        float r2m = g0.y * g0.y;
        float f0 = fma(g0.z, r2m * -0.159999996, r2m * 0.159999996);
        float ce = e == 0.0 ? 0.00999999978 : e;
        float sc = k6.z;
        spec = (((pre * sc) * fma(fma(vec3(g0.z), base, vec3(f0)), vec3(lut.x), vec3(lut.y))) * 100.0) * ce;
        diff = ((fma(vec3(g0.z), -base, base) * (lut.z * (irr * sc))) * 100.0) * ce;
    }
    outSpec = vec4(spec, 0.0);
    outDiff = vec4(diff, 0.0);
}
