#version 450
// [DOS2] VK-SHADOWS-MOVING (dm/gfx/vkn/vkn_shs.c): the scrolled shadow layer into a cascade tile. The layer is addressed toroidally
// (layer texel of tile pixel p = tile origin + (p + off) mod tile size) and holds depths in the z frame of its build; par.x = the
// frame's z shift (+ a quarter of a D16 step: the same texel whether the conversion rounds or truncates). A texel at the clear value
// (par.y) holds no caster: discarded (the tile keeps its own). Drawn with LESS + depth writes: order-free like the casters.
// glslc --target-env=vulkan1.0 -O -fshader-stage=frag -mfmt=num -o vkn_shs_comp.frag.spv.inc vkn_shs_comp.frag
layout(set = 0, binding = 0) uniform U { ivec4 tile; ivec4 off; vec4 par; } u;
layout(set = 0, binding = 1) uniform sampler2D layer;
void main() {
    ivec2 q = ivec2(gl_FragCoord.xy) - u.tile.xy + u.off.xy;
    q -= u.tile.zw * ivec2(greaterThanEqual(q, u.tile.zw));
    float d = texelFetch(layer, u.tile.xy + q, 0).r;
    if (d >= u.par.y) discard;
    gl_FragDepth = d + u.par.x;
}
