#version 450
#extension GL_GOOGLE_include_directive : require
// DOS2 LinearizeDepth (fragment key f23c09d198b55dc5): d = t_8.x at TexCoord0.xy; 0 and 1 pass through, else (1 / (d - c3[0].x)) * c3[0].y;
// out (r, 0, 0, 0). Interface = the translated bundle's: 16 fp_c3, 10 t_8; in location 1 noperspective; out location 0.
#include "common.glsl"
CBUF(16, c3)
layout(set = 0, binding = 10) uniform sampler2D tDepth;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

void main()
{
    float d = texture(tDepth, nanz(TexCoord0.xy)).x;
    vec2 k = c3._m0[0].xy;
    float r = (d == 0.0 || d == 1.0) ? d : (1.0 / (d - k.x)) * k.y;
    outColor = vec4(r, 0.0, 0.0, 0.0);
}
