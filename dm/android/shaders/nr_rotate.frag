#version 450
// [DOS2] NR-20: the window texture, nearest (the old blit's filter) or linear (TL2 pillarbox squeeze), through the sampler of the set.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D tex;
void main() { color = texture(tex, uv); }
