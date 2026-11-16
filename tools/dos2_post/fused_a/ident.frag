#version 450
// NB-18b-8 F1A, control build only: the colour producer "the texel itself" (instead of PPApplyLUT). With LUMA 1 the rebuilt FXAA
// must then equal the translated PPFXAA - the test that the surgery of tools/dos2_post_fuse.py kept its arithmetic.
layout(binding = 64) uniform sampler2D base;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(base, ivec2(gl_FragCoord.xy), 0); }
