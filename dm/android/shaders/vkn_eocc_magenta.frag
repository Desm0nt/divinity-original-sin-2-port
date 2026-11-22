#version 450
// [DOS2] VK-ENGINE-SIDE I2 poison (dm/gfx/vkn/vkn_eocc.h, DM_VK_ENG_OCC=3): the fragment stage of the twin of an engine shader drawn
// for the G-buffer draws of an object the occlusion test would hide: every colour target magenta (albedo, normals, material: the lit
// result reads as a magenta surface). The vertex stage is the engine's own module.
// Build: glslc -fshader-stage=frag --target-env=vulkan1.0 -O vkn_eocc_magenta.frag -mfmt=num -o vkn_eocc_magenta.frag.spv.inc
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
void main() {
    o0 = vec4(1.0, 0.0, 1.0, 1.0);
    o1 = vec4(1.0, 0.0, 1.0, 1.0);
    o2 = vec4(1.0, 0.0, 1.0, 1.0);
    o3 = vec4(1.0, 0.0, 1.0, 1.0);
}
