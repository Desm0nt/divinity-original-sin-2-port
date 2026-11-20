#version 450
#extension GL_GOOGLE_include_directive : require
// DOS2 PPBloomUpsampleAndBlur (fragment key 4e290c9cf1162fe9): bloom upsample (R11G11B10F). r = 1 / c15[13].zw (source texel size);
//   a = t_A sampled (lod 0) at uv = TexCoord0.xy and at uv -+ 2 r.x, uv -+ 2 r.y (fma(r, -+2, uv)); blur = 0.125 * a(-x) + 0.5 * a(0)
//   + 0.125 * (a(-y) + a(+x) + a(+y)) as the translation's fma chain; out = fma(blur, c8[0].x, t_B(uv)) (bloom over the finer level).
// Same products and fma order as the translated module (float for float), vec4 ops instead of its scalar chain; same 6 samples.
// Interface = the translated bundle's: 22 fp_c15, 28 fp_c8, 10 t_A (coarser bloom), 16 t_8 (finer level); in location 1
// noperspective vec4; out location 0 (binding 4 support buffer unused, as in the translated module).
#include "common.glsl"
CBUF(22, c15)
CBUF(28, c8)
layout(set = 0, binding = 10) uniform sampler2D tCoarse;
layout(set = 0, binding = 16) uniform sampler2D tFine;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = TexCoord0.xy;
    vec2 r = 1.0 / c15._m0[13].zw;
    vec2 lo = fma(r, vec2(-2.0), uv), hi = fma(r, vec2(2.0), uv);
    vec2 c = nanz(uv);
    vec4 b = textureLod(tCoarse, nanz(vec2(lo.x, uv.y)), 0.0) * 0.125;
    b = fma(textureLod(tCoarse, c, 0.0), vec4(0.5), b);
    b = fma(textureLod(tCoarse, nanz(vec2(uv.x, lo.y)), 0.0), vec4(0.125), b);
    b = fma(textureLod(tCoarse, nanz(vec2(hi.x, uv.y)), 0.0), vec4(0.125), b);
    b = fma(textureLod(tCoarse, nanz(vec2(uv.x, hi.y)), 0.0), vec4(0.125), b);
    outColor = fma(b, vec4(c8._m0[0].x), textureLod(tFine, c, 0.0));
}
