#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 EoCOutlineCombine (fragment key 010472595b342188): composites the blurred outline over the scene. Per pixel:
//   m = alpha of t_C (outline mask, texelFetch), s = t_8 (scene, texelFetch), o = t_A (blurred outline, sampled at TexCoord0.xy);
//   cm = ceil(m); k = sat(o.a - cm); base = fma(o.rgb - s, k, s); edge = ceil(fma(m, -cm, cm));
//   out.rgb = fma((o.rgb - base) * edge, c1[0].x, base) (c1[0].x = 0.15 in the bundle), out.a = 1.
// Interface = the translated bundle's: 4 support buffer, 28 fp_c1 (immediate constants), 10 t_C, 16 t_8, 22 t_A;
// in gl_FragCoord + location 1 noperspective vec4; out location 0. Fast path for render scale 1 (host default), general path exact.
#define NEED_RS
#include "common.glsl"
SUPPORT_BUFFER(4)
CBUF(28, c1)
layout(set = 0, binding = 10) uniform sampler2D tMask;
layout(set = 0, binding = 16) uniform sampler2D tScene;
layout(set = 0, binding = 22) uniform sampler2D tOutline;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

void main()
{
    float s0 = sb._m5[0], sm = sb._m5[1], ss = sb._m5[2];
    ivec2 i, im, is;
    [[dont_flatten]] if (s0 == 1.0 && sm == 1.0 && ss == 1.0) { i = ivec2(gl_FragCoord.xy); im = i; is = i; }
    else { i = ivec2(trunc(gl_FragCoord.xy / s0)); im = rs_fix2(i, sm); is = rs_fix2(i, ss); }
    float m = texelFetch(tMask, im, 0).w;
    vec3 s = texelFetch(tScene, is, 0).rgb;
    vec4 o = texture(tOutline, nanz(TexCoord0.xy));
    float cm = ceil(m);
    float k = clamp(o.a - cm, 0.0, 1.0);
    float edge = ceil(fma(m, -cm, cm));
    vec3 base = fma(o.rgb - s, vec3(k), s);
    outColor = vec4(fma((o.rgb - base) * edge, vec3(c1._m0[0].x), base), 1.0);
}
