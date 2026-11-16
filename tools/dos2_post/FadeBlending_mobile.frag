#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 FadeBlending (EngineShaders PostProcess FadeBlending, fragment key 62d683cc49953ad6): full frame, every frame, R11G11B10.
// Checkerboard fade of the scene colour through the fade mask: per pixel the diagonal pair B of its 2x2 quad (B = the anti-diagonal on
// even pixels, the main diagonal on odd ones) is read from the mask (alpha of t_A); where the mask average is strictly inside (0, 1)
// the colour is 0.5 * fma(s, 0.5 * B - 0.5 * A, A) (A, B = colour sums of the two diagonals, s = mask sum), else the pixel's own colour;
// alpha = the colour target's alpha at the unclamped pixel. Derived from the translated SPIR-V (spirv-opt -O).
// Interface = the translated bundle's: binding 4 support buffer, 22 fp_c15 (c15[13].zw = target size), 10 t_A (mask), 16 t_8 (colour);
// in gl_FragCoord; out location 0.
// Fast path (uniform branch): render scales _m5[0..2] all 1 (what the host writes): 3 fetches per pixel (6 inside the fade band)
// against the Switch 4 (7), no per-fetch scale fix-up. Other scales: the general path = the translated code literally.
#define NEED_RS
#include "common.glsl"
SUPPORT_BUFFER(4)
CBUF(22, c15)
layout(set = 0, binding = 10) uniform sampler2D tMask;
layout(set = 0, binding = 16) uniform sampler2D tColor;
layout(location = 0) out vec4 outColor;

vec3 blend(float s, vec3 c00, vec3 c11, vec3 c01, vec3 c10, bool odd)
{
    vec3 d0 = c00 + c11, d1 = c01 + c10;
    vec3 a = odd ? d1 : d0, b = odd ? d0 : d1;
    return fma(vec3(s), fma(b, vec3(0.5), a * -0.5), a) * 0.5;
}

void main()
{
    float s0 = sb._m5[0], sm = sb._m5[1], sc = sb._m5[2];
    vec2 lim = c15._m0[13].zw - 1.0;
    [[dont_flatten]] if (s0 == 1.0 && sm == 1.0 && sc == 1.0) {
        ivec2 i = ivec2(gl_FragCoord.xy);
        ivec2 m = ivec2(lim);
        ivec2 a0 = min(i, m), a1 = min(i + 1, m);
        bool odd = ((i.x + i.y) & 1) != 0;
        ivec2 a01 = ivec2(a0.x, a1.y), a10 = ivec2(a1.x, a0.y);
        float s = texelFetch(tMask, odd ? a1 : a01, 0).w + texelFetch(tMask, odd ? a0 : a10, 0).w;
        float h = s * 0.5;
        vec4 c = texelFetch(tColor, a0, 0);
        vec3 rgb = c.rgb;
        [[dont_flatten]] if (h > 0.0 && h < 1.0)
            rgb = blend(s, c.rgb, texelFetch(tColor, a1, 0).rgb, texelFetch(tColor, a01, 0).rgb, texelFetch(tColor, a10, 0).rgb, odd);
        float al = c.w;
        [[dont_flatten]] if (any(notEqual(i, a0))) al = texelFetch(tColor, i, 0).w;
        outColor = vec4(rgb, al);
    } else {
        vec2 f = gl_FragCoord.xy / s0;
        vec2 t = trunc(f);
        uvec2 u = uvec2(max(t, 0.0));
        bool odd = ((u.x + u.y) & 1u) != 0u;
        ivec2 i = ivec2(t), i1 = ivec2(trunc(f + 1.0));
        int ax = int(min(float(i.x), lim.x)), ay = int(min(lim.y, float(i.y)));
        int ax1 = int(min(lim.x, float(i1.x))), ay1 = int(min(float(i1.y), lim.y));
        ivec2 a0 = ivec2(ax, ay), a1 = ivec2(ax1, ay1), a01 = ivec2(ax, ay1), a10 = ivec2(ax1, ay);
        float s = texelFetch(tMask, rs_fix2(odd ? a1 : a01, sm), 0).w + texelFetch(tMask, rs_fix2(odd ? a0 : a10, sm), 0).w;
        float h = s * 0.5;
        vec3 rgb;
        if (h > 0.0 && h < 1.0)
            rgb = blend(s, texelFetch(tColor, rs_fix2(a0, sc), 0).rgb, texelFetch(tColor, rs_fix2(a1, sc), 0).rgb,
                        texelFetch(tColor, rs_fix2(a01, sc), 0).rgb, texelFetch(tColor, rs_fix2(a10, sc), 0).rgb, odd);
        else
            rgb = texelFetch(tColor, rs_fix2(a0, sc), 0).rgb;
        outColor = vec4(rgb, texelFetch(tColor, rs_fix2(i, sc), 0).w);
    }
}
