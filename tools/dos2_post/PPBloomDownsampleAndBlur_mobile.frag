#version 450
#extension GL_GOOGLE_include_directive : require
// DOS2 PPBloomDownsampleAndBlur (fragment key 0e3fc07b320db1e9): bloom mip chain downsample (R11G11B10F). With r = 1 / c15[13].zw:
//   5 bilinear taps a of t_8 (lod 0) at TexCoord0.xy and at uv.x + (-1 or 1) / size.x, uv.y + (-1 or 1) / size.y;
//   out = 0.125 * a(-x) + 0.5 * a(0) + 0.125 * (a(-y) + a(+x) + a(+y)) as the translation's fma chain (same order).
// Float for float the translated module, vec4 ops instead of its scalar chain; same 5 samples.
// Interface = the translated bundle's: 16 fp_c15, 10 t_8; in location 1 noperspective vec4; out location 0 (binding 4 unused).
#include "common.glsl"
CBUF(16, c15)
layout(set = 0, binding = 10) uniform sampler2D tSrc;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = TexCoord0.xy;
    vec2 sz = c15._m0[13].zw;
    vec2 lo = -1.0 / sz + uv, hi = 1.0 / sz + uv;
    vec4 b = textureLod(tSrc, nanz(vec2(lo.x, uv.y)), 0.0) * 0.125;
    b = fma(textureLod(tSrc, nanz(uv), 0.0), vec4(0.5), b);
    b = fma(textureLod(tSrc, nanz(vec2(uv.x, lo.y)), 0.0), vec4(0.125), b);
    b = fma(textureLod(tSrc, nanz(vec2(hi.x, uv.y)), 0.0), vec4(0.125), b);
    outColor = fma(textureLod(tSrc, nanz(vec2(uv.x, hi.y)), 0.0), vec4(0.125), b);
}
