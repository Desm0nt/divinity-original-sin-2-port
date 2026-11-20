#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 DeferredFog (fragment key d8aeec640d465275): distance + height fog over the HDR scene (R11G11B10F, full frame). Per pixel:
//   d = t_8.x (linear depth) and c = t_A (scene) by texelFetch at the pixel; p = d * Ray.xyz, dist = sqrt(fma(p.z, p.z, fma(p.y, p.y,
//   p.x * p.x))); h = (fma(d, Ray.y, c15[12].y) - K6.z) * (1 / K6.w); m = sat(max(h, sat((K5.w - dist) * (1 / (K5.w - K4.w)))));
//   fog = fma(c.rgb - (K4.y, K5.y, K6.y), m, (K4.y, K5.y, K6.y)); t = sat((K5.z - dist) * (1 / (K5.z - K4.z)));
//   out = (fma(fog - (K4.x, K5.x, K6.x), t, (K4.x, K5.x, K6.x)), c.a)   (K = fp_c16). Derived from the translated SPIR-V (spirv-opt -O);
// same reciprocals, products and fma order, so the result is the same float for float, but vector ops instead of the scalar chain.
// Interface = the translated bundle's: 4 support buffer, 22 fp_c16, 28 fp_c15, 10 t_8 (depth), 16 t_A (scene);
// in gl_FragCoord + location 2 noperspective vec4 (view ray); out location 0.
// Fast path (uniform branch): render scales _m5[0..2] all 1 (what the host writes), no per-fetch scale fix-up; else the translated
// coordinate code literally (fragment coordinate / _m5[0], fix-up by _m5[1] for t_8, _m5[2] for t_A).
#define NEED_RS
#include "common.glsl"
SUPPORT_BUFFER(4)
CBUF(22, c16)
CBUF(28, c15)
// NR-ONCHIP S2F (docs/dos2/nr/NR-FRAME.md §8): with OC != 0 (only tools/dos2_onchip_shaders.py defines it; the pack build of x.DeferredFog
// never does) texture n with bit n of OC set is an input attachment (input_attachment_index n): 0 tDepth, 1 tScene (= the lit image Df the
// light subpass of the merged pass wrote). Only at render scale 1 (the host's value): every fetch is at the fragment's own pixel.
#ifndef OC
#define OC 0
#endif
#if (OC >> 0) & 1
layout(input_attachment_index = 0, set = 0, binding = 10) uniform subpassInput tDepth;
#define F_DEPTH(p) subpassLoad(tDepth)
#else
layout(set = 0, binding = 10) uniform sampler2D tDepth;
#define F_DEPTH(p) texelFetch(tDepth, p, 0)
#endif
#if (OC >> 1) & 1
layout(input_attachment_index = 1, set = 0, binding = 16) uniform subpassInput tScene;
#define F_SCENE(p) subpassLoad(tScene)
#else
layout(set = 0, binding = 16) uniform sampler2D tScene;
#define F_SCENE(p) texelFetch(tScene, p, 0)
#endif
layout(location = 2) noperspective in vec4 Ray;
layout(location = 0) out vec4 outColor;

void main()
{
    float d;
    vec4 c;
#if OC
    {   // render scale 1 only
        ivec2 i = ivec2(gl_FragCoord.xy);
        d = F_DEPTH(i).x;
        c = F_SCENE(i);
    }
#else
    float s0 = sb._m5[0], sd = sb._m5[1], sc = sb._m5[2];
    [[dont_flatten]] if (s0 == 1.0 && sd == 1.0 && sc == 1.0) {
        ivec2 i = ivec2(gl_FragCoord.xy);
        d = texelFetch(tDepth, i, 0).x;
        c = texelFetch(tScene, i, 0);
    } else {
        ivec2 i = ivec2(trunc(gl_FragCoord.xy / s0));
        d = texelFetch(tDepth, rs_fix2(i, sd), 0).x;
        c = texelFetch(tScene, rs_fix2(i, sc), 0);
    }
#endif
    vec4 k4 = c16._m0[4], k5 = c16._m0[5], k6 = c16._m0[6];
    vec3 p = d * Ray.xyz;
    float dist = sqrt(fma(p.z, p.z, fma(p.y, p.y, p.x * p.x)));
    float h = (fma(d, Ray.y, c15._m0[12].y) - k6.z) * (1.0 / k6.w);
    float m = clamp(max(h, clamp((k5.w - dist) * (1.0 / (k5.w - k4.w)), 0.0, 1.0)), 0.0, 1.0);
    vec3 fy = vec3(k4.y, k5.y, k6.y), fx = vec3(k4.x, k5.x, k6.x);
    vec3 fog = fma(c.rgb - fy, vec3(m), fy);
    float t = clamp((k5.z - dist) * (1.0 / (k5.z - k4.z)), 0.0, 1.0);
    outColor = vec4(fma(fog - fx, vec3(t), fx), c.a);
}
