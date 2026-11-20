#version 450
// [DOS2] NR-20 DM_NR_ROTATE (vulkan_present.cpp namespace nrr; the map: gfx/nr/nr_rotate.h nrr_uv_affine). One triangle covers the viewport
// (the destination rectangle inside the pre-rotated swapchain image); the texture coordinate is an affine map of the position normalized
// over the viewport, so the window texture is read turned (idea: nx2android shaders/rot.vert).
layout(push_constant) uniform R { vec4 m; vec2 t; } r;
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // (0,0) (2,0) (0,2): the viewport is the corner [0,1]^2
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    uv = vec2(dot(r.m.xy, p), dot(r.m.zw, p)) + r.t;
}
