#version 450
// NB-18b-8 F1A: an HDR textureGather(G) of FXAA's neighbourhood -> four luma proxies (input 0 = the gather's result).
#include "luma.glsl"
layout(location = 0) in vec4 v;
layout(location = 0) out vec4 o;
void main() { o = PROXY(v); }
