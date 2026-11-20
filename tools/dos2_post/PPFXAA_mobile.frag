#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 PPFXAA (fragment key 691768aa08545d95): FXAA 3.11-style anti-aliasing on G of the RGBA8 image (2160x972 and the HUD portraits).
//   M = t_8 at uv = TexCoord0.xy; S, SE, E = textureGather(G) at uv, W, N, NW = textureGatherOffset(-1, -1); early exit (out = M.rgb)
//   when range < max(0, rangeMax / 6). Else NE / SW taps, horizontal / vertical edge choice, a 3-step search each way along the edge
//   (offsets 1.5, 3, 12 texels after the first, 2 taps per step only while that side is not done), the FXAA 3.11 subpixel blend with
//   c1[0].x (1/12) and c1[0].y (0.5) and the int c1[0].z (1) as the goodSpan flag, a final bilinear tap of t_8 (rgb out, alpha 1).
// Transcribed from the translated SPIR-V (spirv-opt -O) operation for operation: same fma / mul / add order, the same comparisons
// including the translation's NaN forms (an unordered "a < b || isnan(a) || isnan(b)" is written !(a >= b)), and the places where the
// translation uses the ordered compare instead (edge choice of the W / N luma, the first B-side step), so the result is the same float
// for float (also with NaN texels / coordinates: every sample coordinate goes through nanz as there).
// Interface = the translated bundle's: 16 fp_c15 (c15[13].zw = target size), 22 fp_c1 (immediate constants), 10 t_8; in location 1
// noperspective vec4; out location 0 (binding 4 support buffer unused, as in the translated module).
#include "common.glsl"
CBUF(16, c15)
CBUF(22, c1)
layout(set = 0, binding = 10) uniform sampler2D tSrc;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

float g(float x, float y) { return textureLod(tSrc, nanz(vec2(x, y)), 0.0).y; }

void main()
{
    float u = TexCoord0.x, v = TexCoord0.y;
    vec2 c = nanz(vec2(u, v));
    vec3 cm = textureLod(tSrc, c, 0.0).rgb;
    float M = cm.g;
    vec4 ga = textureGather(tSrc, c, 1), gb = textureGatherOffset(tSrc, c, ivec2(-1), 1);
    float S = ga.x, SE = ga.y, E = ga.z, W = gb.x, N = gb.z, NW = gb.w;
    float rmax = max(max(E, max(S, M)), max(W, N));
    float range = rmax - min(min(W, N), min(E, min(S, M)));
    [[dont_flatten]] if (range < max(0.0, rmax * 0.16666667)) { outColor = vec4(cm, 1.0); return; }
    float NE = textureLodOffset(tSrc, c, 0.0, ivec2(1, -1)).y, SW = textureLodOffset(tSrc, c, 0.0, ivec2(-1, 1)).y;
    float we = E + W, ns = S + N;
    float ry = 1.0 / c15._m0[13].w;
    float nwne = NW + NE, sene = SE + NE, sesw = SE + SW, nwsw = NW + SW;
    float ea = fma(abs(fma(M, -2.0, we)), 2.0, abs(fma(N, -2.0, nwne))) + abs(fma(S, -2.0, sesw));
    float eb = fma(abs(fma(M, -2.0, ns)), 2.0, abs(fma(E, -2.0, sene))) + abs(fma(W, -2.0, nwsw));
    float rx = 1.0 / c15._m0[13].z;
    bool vert = eb >= ea, horz = !vert;
    float l1 = eb < ea ? W : N, l2 = horz ? E : S;
    float sx = horz ? 0.0 : rx, sy = horz ? ry : 0.0;
    float g1 = abs(l1 - M), g2 = abs(l2 - M);
    bool pairP = g1 >= g2;
    float len = vert ? ry : rx;
    if (pairP) len = -len;
    float vb = vert ? fma(len, 0.5, v) : v, ub = horz ? fma(len, 0.5, u) : u;
    float yP = sy + vb, xP = sx + ub, xN = ub - sx, yN = vb - sy;
    float tP = g(xP, yP), tN = g(xN, yN);
    float lnn = pairP ? M + l1 : M + l2;
    float gs = max(g1, g2) * 0.25;
    float t = clamp((1.0 / range) * abs(fma(fma(ns + we, 2.0, sene + nwsw), c1._m0[0].x, -M)), 0.0, 1.0);
    float sp = fma(t, -2.0, 3.0) * (t * t);
    bool ltM = M < lnn * 0.5;
    float eA = fma(lnn, -0.5, tP), eB = fma(lnn, -0.5, tN);
    bool nA = !(abs(eA) >= gs), ltB = abs(eB) < gs, nB = !(abs(eB) >= gs);
    float xA = nA ? fma(sx, 1.5, xP) : xP, yA = nA ? fma(sy, 1.5, yP) : yP;
    float xB = ltB ? fma(sx, -1.5, xN) : xN, yB = nB ? fma(sy, -1.5, yN) : yN;
    [[dont_flatten]] if (nB || nA) {
        if (nB) eB = fma(lnn, -0.5, g(xB, yB));
        if (nA) eA = fma(lnn, -0.5, g(xA, yA));
        float aB = abs(eB), aA = abs(eA);
        bool ltB2 = aB < gs;
        nB = !(aB >= gs); nA = !(aA >= gs);
        if (ltB2) xB = fma(sx, -3.0, xB);
        if (nB) yB = fma(sy, -3.0, yB);
        if (nA) { xA = fma(sx, 3.0, xA); yA = fma(sy, 3.0, yA); }
        [[dont_flatten]] if (nB || nA) {
            if (nB) eB = fma(lnn, -0.5, g(xB, yB));
            if (nA) eA = fma(lnn, -0.5, g(xA, yA));
            aB = abs(eB); aA = abs(eA);
            if (!(aB >= gs)) yB = fma(sy, -12.0, yB);
            if (aB < gs) xB = fma(sx, -12.0, xB);
            if (!(aA >= gs)) { xA = fma(sx, 12.0, xA); yA = fma(sy, 12.0, yA); }
        }
    }
    float dN = horz ? v - yB : u - xB, dP = horz ? yA - v : xA - u;
    int one = floatBitsToInt(c1._m0[0].z), im = ltM ? 1 : 0;
    int iA = eA < 0.0 ? one : 0, iB = eB < 0.0 ? one : 0;
    bool good = dN < dP ? iB != im : iA != im;
    float po = good ? fma(1.0 / (dP + dN), -min(dP, dN), c1._m0[0].y) : 0.0;
    float m = max((sp * sp) * 0.75, po);
    float uf = horz ? fma(m, len, u) : u, vf = vert ? fma(m, len, v) : v;
    outColor = vec4(textureLod(tSrc, nanz(vec2(uf, vf)), 0.0).rgb, 1.0);
}
