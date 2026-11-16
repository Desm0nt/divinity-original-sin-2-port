#version 450
#extension GL_GOOGLE_include_directive : require
// DOS2 PPDownSampleLog2 (fragment key cbe8649300e9c8cd): first bloom / luminance downsample. c = t_8 at TexCoord0.xy;
// v = (r, g, b all > 2) ? log2(rgb) : rgb * 0.5; any NaN or infinity in (v, c.a) -> (0, 0, 0, 1), else (v, c.a).
// Infinity is tested as the translation does (x != 0 && x == 2x). Interface: 10 t_8; in location 1 noperspective; out location 0.
#include "common.glsl"
layout(set = 0, binding = 10) uniform sampler2D tSrc;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 c = texture(tSrc, nanz(TexCoord0.xy));
    vec3 v = any(lessThanEqual(c.rgb, vec3(2.0))) ? c.rgb * 0.5 : log2(c.rgb);
    vec4 o = vec4(v, c.a);
    bvec4 nz = notEqual(o, vec4(0.0)), dbl = equal(o, o * 2.0);
    bool bad = any(isnan(o)) || any(bvec4(nz.x && dbl.x, nz.y && dbl.y, nz.z && dbl.z, nz.w && dbl.w));
    outColor = bad ? vec4(0.0, 0.0, 0.0, 1.0) : o;
}
