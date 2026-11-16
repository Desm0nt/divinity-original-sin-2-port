#version 450
// NB-18b-8 F1A: FXAA's last tap. The console samples its (post-LUT) input bilinearly at uv = the pixel centre moved along ONE axis by
// |d| <= 0.75 texel: a mix of the pixel's own texel M and its neighbour N across the edge. Here the input is the pre-LUT HDR image, so the
// colours are mixed AFTER the LUT ("A-exact": mix(LUT(M), LUT(N), w)). cm = LUT(M), the centre colour the rebuilt FXAA already has;
// B is a placeholder whose read tools/dos2_post_fuse.py replaces with the translated PPApplyLUT run at the given pixel position (its
// gl_FragCoord) - executed only when the weight is not zero.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 cm;
layout(binding = 64) uniform sampler2D base;    // the HDR image (shared with the analysis taps): its size
layout(binding = 206) uniform sampler2D B;
layout(location = 0) out vec4 o;
void main() {
    vec2 size = vec2(textureSize(base, 0));
    vec2 pm = gl_FragCoord.xy;
    vec2 d = uv * size - pm, ad = abs(d);
    float w = clamp(max(ad.x, ad.y), 0.0, 1.0);
    vec2 st = ad.x >= ad.y ? vec2(d.x < 0.0 ? -1.0 : 1.0, 0.0) : vec2(0.0, d.y < 0.0 ? -1.0 : 1.0);
    vec4 c = cm;
    if (w > 1.0 / 512.0) c = mix(c, textureLod(B, clamp(pm + st, vec2(0.5), size - 0.5), 0.0), w);
    o = c;
}
