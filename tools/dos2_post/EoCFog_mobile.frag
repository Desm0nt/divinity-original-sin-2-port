#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 EoCFog (fragment key e94ea226e689801c): fog over the HDR scene.
//   d = t_A.x (linear depth) and col = t_8.rgb at TexCoord0.xy. Sky (d >= 1): fma(col, -c8[0].w, col).
//   Else world xz = fma(d, ray.xz, c15[12].xz) (ray = location 2); fog map t_E at ((x - c8[2].x) * (1/c8[1].z) * (1/c8[1].x),
//   (z - c8[2].y) * (1/c8[1].z) * (1/c8[1].y)), f = sat(|fma(t_E.x, 255, -c8[1].w)|); when c8[0].w > 0 col is first multiplied by a
//   second map t_C at ((x - c8[2].x) * (1/c8[0].z) * (1/c8[0].x), (z - c8[2].y) * (1/c8[0].z) * (1/c8[0].y)); out = fma(f, -col, col), a 1.
// Same reciprocals, products and fma order as the translated module, so the result is the same float for float.
// Interface = the translated bundle's: 34 fp_c8, 40 fp_c15, 10 t_A, 16 t_8, 22 t_E, 28 t_C; in locations 1, 2 noperspective; out 0.
#include "common.glsl"
CBUF(34, c8)
CBUF(40, c15)
layout(set = 0, binding = 10) uniform sampler2D tDepth;
layout(set = 0, binding = 16) uniform sampler2D tScene;
layout(set = 0, binding = 22) uniform sampler2D tFog;
layout(set = 0, binding = 28) uniform sampler2D tCloud;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 2) noperspective in vec4 Ray;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = nanz(TexCoord0.xy);
    float d = texture(tDepth, uv).x;
    vec3 col = texture(tScene, uv).rgb;
    vec4 k0 = c8._m0[0];
    vec3 o;
    [[dont_flatten]] if (d < 1.0) {
        vec4 k1 = c8._m0[1], k2 = c8._m0[2];
        vec4 vp = c15._m0[12];
        float dx = fma(d, Ray.x, vp.x) - k2.x, dz = fma(d, Ray.z, vp.z) - k2.y;
        float r1 = 1.0 / k1.z;
        vec2 fuv = vec2((dx * r1) * (1.0 / k1.x), (dz * r1) * (1.0 / k1.y));
        float f = textureLod(tFog, nanz(fuv), 0.0).x;
        [[dont_flatten]] if (0.0 < k0.w) {
            float q = 1.0 / k0.z;
            vec2 cuv = vec2((dx * q) * (1.0 / k0.x), (dz * q) * (1.0 / k0.y));
            col *= textureLod(tCloud, nanz(cuv), 0.0).x;
        }
        float a = clamp(abs(fma(f, 255.0, -k1.w)), 0.0, 1.0);
        o = fma(vec3(a), -col, col);
    } else {
        o = fma(col, vec3(-k0.w), col);
    }
    outColor = vec4(o, 1.0);
}
