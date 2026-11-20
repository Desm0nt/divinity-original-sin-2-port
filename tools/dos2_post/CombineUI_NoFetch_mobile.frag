#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 CombineUI_NoFetch (fragment key 98186c22a7209861): UI layer over the tone-mapped scene into the swap-chain image (RGBA8).
//   u = t_A by texelFetch at int(trunc(fragcoord / _m5[0] - c8[0].xy)) (render-scale fix-up by _m5[1]); r = 1 / c8[1].y (gamma);
//   u.a < c1[0].x (0.001 in the bundle): out = t_8(TexCoord0.xy).rgb (no UI); u.a == 1: out = exp2(r * log2(sat(u.rgb)));
//   else s = t_8(TexCoord0.xy): out = exp2(r * log2(sat(u.rgb + fma(u.a, -s.rgb, s.rgb)))) (premultiplied UI over the scene); a = 1.
// The translated module always samples t_8 once (at (0, 0) when u.a >= c1[0].x, a result it never uses) and samples it again at
// TexCoord0.xy for a translucent UI pixel; this one samples t_8 only where it is used, once: 1 texture op on opaque UI pixels, 2 elsewhere
// (translated 2 / 3). Same products and order as the translated module (float for float).
// Interface = the translated bundle's: 4 support buffer, 22 fp_c8, 28 fp_c1 (immediate constants), 10 t_A (UI), 16 t_8 (scene);
// in gl_FragCoord + location 1 noperspective vec4; out location 0.
#define NEED_RS
#include "common.glsl"
SUPPORT_BUFFER(4)
CBUF(22, c8)
CBUF(28, c1)
layout(set = 0, binding = 10) uniform sampler2D tUI;
layout(set = 0, binding = 16) uniform sampler2D tScene;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

void main()
{
    float s0 = sb._m5[0], su = sb._m5[1];
    ivec2 i = ivec2(trunc(gl_FragCoord.xy / s0 - c8._m0[0].xy));
    [[dont_flatten]] if (su != 1.0) i = rs_fix2(i, su);
    vec4 u = texelFetch(tUI, i, 0);
    float a = u.a;
    vec3 o;
    bool none = a < c1._m0[0].x;
    [[dont_flatten]] if (!none && a == 1.0) {
        o = exp2((1.0 / c8._m0[1].y) * log2(clamp(u.rgb, 0.0, 1.0)));
    } else {
        vec3 s = textureLod(tScene, nanz(TexCoord0.xy), 0.0).rgb;
        [[dont_flatten]] if (none) o = s;
        else o = exp2((1.0 / c8._m0[1].y) * log2(clamp(u.rgb + fma(vec3(a), -s, s), 0.0, 1.0)));
    }
    outColor = vec4(o, 1.0);
}
