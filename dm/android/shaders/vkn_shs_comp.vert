#version 450
// [DOS2] VK-SHADOWS-MOVING (dm/gfx/vkn/vkn_shs.c): the scrolled shadow layer into a cascade tile - one triangle over the viewport.
// glslc --target-env=vulkan1.0 -O -fshader-stage=vert -mfmt=num -o vkn_shs_comp.vert.spv.inc vkn_shs_comp.vert
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);
}
