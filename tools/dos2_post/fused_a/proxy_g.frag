#version 450
// NB-18b-8 F1A: one bilinear HDR tap of FXAA's edge search -> its luma proxy (the analysis reads .g only). Spliced by
// tools/dos2_post_fuse.py over every G-only OpImageSampleExplicitLod of the translated PPFXAA (input 0 = that tap's result).
#include "luma.glsl"
layout(location = 0) in vec4 v;
layout(location = 0) out vec4 o;
void main() { o = vec4(PROXY(v.g)); }
