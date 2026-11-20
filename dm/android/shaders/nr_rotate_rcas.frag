#version 450
// [DOS2] NR-20 + RCAS (docs/dos2/nr/NR-20-rcas.md; vulkan_present.cpp namespace nrr; the map: gfx/nr/nr_rotate.h nrr_rcas_map). The turned
// draw that also sharpens: RCAS, the sharpening pass of AMD FidelityFX Super Resolution 1.0 (MIT, Copyright (c) 2021 Advanced Micro Devices,
// Inc.; docs/THIRD_PARTY.md), the same filter as rcas.comp, evaluated at the source texel this pre-rotated image pixel shows. One pass
// instead of RCAS into its own image + the blit (+ the compositor's turn). Vertex stage: nr_rotate.vert (its uv is not used here).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    layout(offset = 32) ivec4 m;   // q = (m.x a + m.y b, m.z a + m.w b) + o.zw, (a, b) = this pixel - o.xy
    ivec4 o;                       // o.xy: the viewport's origin in the image; o.zw: q's offset
    ivec4 s;                       // s.xy: the source rectangle's origin; s.zw: the texture's size (the filter's clamp)
    vec2 k;                        // source texels per window pixel (the old blit's scale)
    float sharpness;               // exp2(-stops), 1 = strongest
} p;
layout(location = 0) out vec4 color;
const float RCAS_LIMIT = 0.25 - 1.0 / 16.0;
vec3 at(ivec2 q) { return texelFetch(src, clamp(q, ivec2(0), p.s.zw - 1), 0).rgb; }
void main() {
    ivec2 ab = ivec2(gl_FragCoord.xy) - p.o.xy;
    ivec2 q = ivec2(p.m.x * ab.x + p.m.y * ab.y, p.m.z * ab.x + p.m.w * ab.y) + p.o.zw;
    ivec2 t = clamp(p.s.xy + ivec2(floor((vec2(q) + 0.5) * p.k)), ivec2(0), p.s.zw - 1);
    //    b
    //  d e f
    //    h
    vec3 b = at(t + ivec2(0, -1)), d = at(t + ivec2(-1, 0)), e = at(t), f = at(t + ivec2(1, 0)), h = at(t + ivec2(0, 1));
    vec3 mn4 = min(min(b, d), min(f, h));
    vec3 mx4 = max(max(b, d), max(f, h));
    vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1e-5));
    vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1e-5));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * p.sharpness;
    vec3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
