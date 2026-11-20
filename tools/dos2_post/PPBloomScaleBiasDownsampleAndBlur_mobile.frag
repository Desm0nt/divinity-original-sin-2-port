#version 450
#extension GL_GOOGLE_include_directive : require
// DOS2 PPBloomScaleBiasDownsampleAndBlur (fragment key 74fe9ae2066d53da): first bloom level, bright-pass + 4-tap downsample (R11G11B10F).
//   r = 1 / c15[13].zw; 4 bilinear taps t of t_8 (lod 0) at fma(r, (-+0.25, -+0.25), TexCoord0.xy) in the translated order
//   (-x +y, -x -y, +x -y, +x +y); per tap l = fma(t.b, c1[0].y, fma(t.g, c1[0].x, t.r * 0.3)) (c1 = 0.59, 0.11 in the bundle),
//   k = sat((l - c8[0].x) * c8[0].y); out.rgb = min over the taps of t.rgb * k, out.a = min over the taps of t.a (same min order).
// Same products / fma / min order as the translated module (float for float), vec3 ops instead of its scalar chain; same 4 samples.
// Interface = the translated bundle's: 16 fp_c15, 22 fp_c1 (immediate constants), 28 fp_c8, 10 t_8; in location 1 noperspective
// vec4; out location 0 (binding 4 support buffer unused, as in the translated module).
#include "common.glsl"
CBUF(16, c15)
CBUF(22, c1)
CBUF(28, c8)
layout(set = 0, binding = 10) uniform sampler2D tSrc;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

vec3 tap(vec4 t, vec2 w, vec2 sb)
{
    float k = clamp((fma(t.b, w.y, fma(t.g, w.x, t.r * 0.3)) - sb.x) * sb.y, 0.0, 1.0);
    return t.rgb * k;
}

void main()
{
    vec2 uv = TexCoord0.xy;
    vec2 r = 1.0 / c15._m0[13].zw;
    vec2 lo = fma(r, vec2(-0.25), uv), hi = fma(r, vec2(0.25), uv);
    vec4 a = textureLod(tSrc, nanz(vec2(lo.x, hi.y)), 0.0), b = textureLod(tSrc, nanz(lo), 0.0);
    vec4 c = textureLod(tSrc, nanz(vec2(hi.x, lo.y)), 0.0), d = textureLod(tSrc, nanz(hi), 0.0);
    vec2 w = c1._m0[0].xy, sb = c8._m0[0].xy;
    vec3 o = min(min(min(tap(a, w, sb), tap(b, w, sb)), tap(c, w, sb)), tap(d, w, sb));
    outColor = vec4(o, min(min(min(a.a, b.a), c.a), d.a));
}
