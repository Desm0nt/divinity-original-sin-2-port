#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// [DOS2] N6 DM_HACK_FSR (docs/dos2/nr/NR-HACKS.md): the PPFXAA step (fragment key 691768aa08545d95) with FSR 1 EASU for a lower 3D scale.
// The console's post chain draws the 3D at the DRS scale s into the top-left s x s part of its 2160x972 targets; the FXAA step is the pass
// that stretches it back to the full target (its vertex shader: TexCoord0.xy = uv * c3[0].xy + c3[0].zw, c3[0].xy = s; NB-18b-11). At s = 1
// (and for the HUD portraits, 1:1) this module is PPFXAA_mobile.frag word for word (proven bit for bit against the translated PPFXAA by
// tools/dos2_post_mobile.py); at s < 1 it runs AMD FidelityFX Super Resolution 1.0 EASU (edge-adaptive spatial upsampling, 12 taps, the
// fp32 FsrEasuF of ffx_fsr1.h, MIT licence, re-written in GLSL) instead of FXAA + the bilinear stretch. RCAS (the second FSR pass) is the
// port's sharpening at present (DM_GFX_SHARPEN, the gear's "Резкость"; NR-20-rcas: inside the turned draw). EASU wants an anti-aliased
// input: at s < 1 this step's FXAA is replaced, so edges keep the low-resolution steps EASU reconstructs (documented as lossy).
// The stretch ratio = d(TexCoord0.x)/dx x the source width (uniform across the draw: TexCoord0 is linear in the full-screen triangle);
// |ratio - 1| < 1/1024 -> FXAA. The 4x4 footprint is clamped to the drawn part (s x the source size, the output target being the source's
// size): texels beyond it hold an older frame.
// Built twice (tools/dos2_post_fsr.py): fp32 FsrEasuF (ppfxaa_fsr.frag.spv.inc) and -DEASU_H the half-precision FsrEasuH form
// (ppfxaa_fsrh.frag.spv.inc, mediump: fp16 ALU where the GPU has it; DM_HACK_FSR=1, the default of the hack).
// Interface = the translated bundle's (as PPFXAA_mobile.frag): 16 fp_c15, 22 fp_c1, 10 t_8; in location 1 noperspective vec4; out location 0.
#include "common.glsl"
CBUF(16, c15)
CBUF(22, c1)
layout(set = 0, binding = 10) uniform sampler2D tSrc;
layout(location = 1) noperspective in vec4 TexCoord0;
layout(location = 0) out vec4 outColor;

float g(float x, float y) { return textureLod(tSrc, nanz(vec2(x, y)), 0.0).y; }

// ---- PPFXAA_mobile.frag main(), unchanged (the 1:1 path)
vec4 fxaa()
{
    float u = TexCoord0.x, v = TexCoord0.y;
    vec2 c = nanz(vec2(u, v));
    vec3 cm = textureLod(tSrc, c, 0.0).rgb;
    float M = cm.g;
    vec4 ga = textureGather(tSrc, c, 1), gb = textureGatherOffset(tSrc, c, ivec2(-1), 1);
    float S = ga.x, SE = ga.y, E = ga.z, W = gb.x, N = gb.z, NW = gb.w;
    float rmax = max(max(E, max(S, M)), max(W, N));
    float range = rmax - min(min(W, N), min(E, min(S, M)));
    [[dont_flatten]] if (range < max(0.0, rmax * 0.16666667)) { return vec4(cm, 1.0); }
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
    return vec4(textureLod(tSrc, nanz(vec2(uf, vf)), 0.0).rgb, 1.0);
}

// ---- FSR 1 EASU (ffx_fsr1.h FsrEasuF, fp32; approximations APrxLoRcpF1 / APrxLoRsqF1 as there)
float rcpLo(float a) { return uintBitsToFloat(0x7ef07ebbu - floatBitsToUint(a)); }
float rsqLo(float a) { return uintBitsToFloat(0x5f347d74u - (floatBitsToUint(a) >> 1u)); }

void easuSet(inout vec2 dir, inout float len, vec2 pp, float w, float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC, cb = lC - lB;
    float lenX = rcpLo(max(abs(dc), abs(cb)));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC, ca = lC - lA;
    float lenY = rcpLo(max(abs(ec), abs(ca)));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void easuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c)
{
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x) * len2;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w; aW += w;
}

// one gather of R, G, B around the texel-corner point q (in texels), clamped to the drawn part (corners 1 .. lim - 1)
void gather3(vec2 q, vec2 lim, vec2 rcp, out vec4 r, out vec4 gg, out vec4 b)
{
    vec2 p = clamp(q, vec2(1.0), lim - 1.0) * rcp;
    r = textureGather(tSrc, p, 0); gg = textureGather(tSrc, p, 1); b = textureGather(tSrc, p, 2);
}

vec4 easu(vec2 ratio)
{
    vec2 size = vec2(textureSize(tSrc, 0)), rcp = 1.0 / size;
    vec2 lim = max(floor(ratio * size + 0.5), vec2(2.0));   // the drawn part in texels (output = source size)
    vec2 pp = TexCoord0.xy * size - 0.5;                     // ffx: ip * con0.xy + con0.zw
    vec2 fp = floor(pp);
    pp -= fp;
    // gather corners (texels): b c / i j f e / k l h g / o n, as p0..p3 of FsrEasuF (con1.zw, con2, con3)
    vec4 bczzR, bczzG, bczzB, ijfeR, ijfeG, ijfeB, klhgR, klhgG, klhgB, zzonR, zzonG, zzonB;
    gather3(fp + vec2(1.0, -1.0), lim, rcp, bczzR, bczzG, bczzB);
    gather3(fp + vec2(0.0, 1.0), lim, rcp, ijfeR, ijfeG, ijfeB);
    gather3(fp + vec2(2.0, 1.0), lim, rcp, klhgR, klhgG, klhgB);
    gather3(fp + vec2(1.0, 3.0), lim, rcp, zzonR, zzonG, zzonB);
    vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x, cL = bczzL.y, iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    float kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w, oL = zzonL.z, nL = zzonL.w;
    vec2 dir = vec2(0.0); float len = 0.0;
    easuSet(dir, len, pp, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easuSet(dir, len, pp, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easuSet(dir, len, pp, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easuSet(dir, len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqLo(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * rcpLo(max(abs(dir.x), abs(dir.y)));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcpLo(lob);
    vec3 fC = vec3(ijfeR.z, ijfeG.z, ijfeB.z), gC = vec3(klhgR.w, klhgG.w, klhgB.w);
    vec3 jC = vec3(ijfeR.y, ijfeG.y, ijfeB.y), kC = vec3(klhgR.x, klhgG.x, klhgB.x);
    vec3 min4 = min(min(fC, min(gC, jC)), kC), max4 = max(max(fC, max(gC, jC)), kC);
    vec3 aC = vec3(0.0); float aW = 0.0;
    easuTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.x, bczzG.x, bczzB.x));   // b
    easuTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, vec3(bczzR.y, bczzG.y, bczzB.y));   // c
    easuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, vec3(ijfeR.x, ijfeG.x, ijfeB.x));   // i
    easuTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);                                 // j
    easuTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);                                 // f
    easuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, vec3(ijfeR.w, ijfeG.w, ijfeB.w));   // e
    easuTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);                                 // k
    easuTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, vec3(klhgR.y, klhgG.y, klhgB.y));   // l
    easuTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, vec3(klhgR.z, klhgG.z, klhgB.z));   // h
    easuTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);                                 // g
    easuTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.z, zzonG.z, zzonB.z));   // o
    easuTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, vec3(zzonR.w, zzonG.w, zzonB.w));   // n
    return vec4(min(max4, max(min4, aC * (1.0 / aW))), 1.0);
}

// ---- FSR 1 EASU, half precision (EASU_H: ffx_fsr1.h FsrEasuH - two taps / two quadrants per packed operation, mediump = RelaxedPrecision:
// fp16 where the GPU has it). Positions stay highp (texel coordinates up to 2160). Reciprocals are exact (ARcpH = 1 / x in ffx_a.h); the
// edge-length reciprocal takes max(.., 2^-14) so 0 / 0 cannot appear (ffx relies on HLSL saturate(NaN) = 0; dirX = dc + cb is 0 there anyway).
#ifdef EASU_H
void easuSetH(inout mediump vec2 dirPX, inout mediump vec2 dirPY, inout mediump vec2 lenP, mediump vec2 w,
              mediump vec2 lA, mediump vec2 lB, mediump vec2 lC, mediump vec2 lD, mediump vec2 lE)
{
    mediump vec2 dc = lD - lC, cb = lC - lB;
    mediump vec2 lenX = 1.0 / max(max(abs(dc), abs(cb)), vec2(1.0 / 16384.0));
    mediump vec2 dirX = lD - lB;
    dirPX += dirX * w;
    lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
    lenX *= lenX;
    lenP += lenX * w;
    mediump vec2 ec = lE - lC, ca = lC - lA;
    mediump vec2 lenY = 1.0 / max(max(abs(ec), abs(ca)), vec2(1.0 / 16384.0));
    mediump vec2 dirY = lE - lA;
    dirPY += dirY * w;
    lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
    lenY *= lenY;
    lenP += lenY * w;
}

void easuTapH(inout mediump vec2 aR, inout mediump vec2 aG, inout mediump vec2 aB, inout mediump vec2 aW, mediump vec2 offX, mediump vec2 offY,
              mediump vec2 dir, mediump vec2 len2, mediump float lob, mediump float clp, mediump vec2 cR, mediump vec2 cG, mediump vec2 cB)
{
    mediump vec2 vX = offX * dir.xx + offY * dir.yy;
    mediump vec2 vY = offX * (-dir.yy) + offY * dir.xx;
    vX *= len2.x; vY *= len2.y;
    mediump vec2 d2 = min(vX * vX + vY * vY, vec2(clp));
    mediump vec2 wB = (2.0 / 5.0) * d2 - 1.0;
    mediump vec2 wA = lob * d2 - 1.0;
    wB *= wB; wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    mediump vec2 w = wB * wA;
    aR += cR * w; aG += cG * w; aB += cB * w; aW += w;
}

void gather3h(vec2 q, vec2 lim, vec2 rcp, out mediump vec4 r, out mediump vec4 gg, out mediump vec4 b)
{
    vec2 p = clamp(q, vec2(1.0), lim - 1.0) * rcp;
    r = textureGather(tSrc, p, 0); gg = textureGather(tSrc, p, 1); b = textureGather(tSrc, p, 2);
}

vec4 easuH(vec2 ratio)
{
    vec2 size = vec2(textureSize(tSrc, 0)), rcp = 1.0 / size;
    vec2 lim = max(floor(ratio * size + 0.5), vec2(2.0));
    vec2 pp = TexCoord0.xy * size - 0.5;
    vec2 fp = floor(pp);
    mediump vec2 ppp = pp - fp;
    mediump vec4 bczzR, bczzG, bczzB, ijfeR, ijfeG, ijfeB, klhgR, klhgG, klhgB, zzonR, zzonG, zzonB;
    gather3h(fp + vec2(1.0, -1.0), lim, rcp, bczzR, bczzG, bczzB);
    gather3h(fp + vec2(0.0, 1.0), lim, rcp, ijfeR, ijfeG, ijfeB);
    gather3h(fp + vec2(2.0, 1.0), lim, rcp, klhgR, klhgG, klhgB);
    gather3h(fp + vec2(1.0, 3.0), lim, rcp, zzonR, zzonG, zzonB);
    mediump vec4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    mediump vec4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    mediump vec4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    mediump vec4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    mediump vec2 dirPX = vec2(0.0), dirPY = vec2(0.0), lenP = vec2(0.0);
    mediump vec2 wx = vec2(1.0, 0.0) + vec2(-ppp.x, ppp.x);
    // quadrants S | T (b e f g j | c f g h k), then U | V (f i j k n | g j k l o)
    easuSetH(dirPX, dirPY, lenP, wx * (1.0 - ppp.y), vec2(bczzL.x, bczzL.y), vec2(ijfeL.w, ijfeL.z), vec2(ijfeL.z, klhgL.w), vec2(klhgL.w, klhgL.z), vec2(ijfeL.y, klhgL.x));
    easuSetH(dirPX, dirPY, lenP, wx * ppp.y, vec2(ijfeL.z, klhgL.w), vec2(ijfeL.x, ijfeL.y), vec2(ijfeL.y, klhgL.x), vec2(klhgL.x, klhgL.y), vec2(zzonL.w, zzonL.z));
    mediump vec2 dir = vec2(dirPX.x + dirPX.y, dirPY.x + dirPY.y);
    mediump float len = lenP.x + lenP.y;
    mediump vec2 dir2 = dir * dir;
    mediump float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : inversesqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    mediump float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    mediump vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    mediump float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    mediump float clp = 1.0 / lob;
    // min and max of f g j k in one packed max: (-min, max)
    mediump vec2 bothR = max(max(vec2(-ijfeR.z, ijfeR.z), vec2(-klhgR.w, klhgR.w)), max(vec2(-ijfeR.y, ijfeR.y), vec2(-klhgR.x, klhgR.x)));
    mediump vec2 bothG = max(max(vec2(-ijfeG.z, ijfeG.z), vec2(-klhgG.w, klhgG.w)), max(vec2(-ijfeG.y, ijfeG.y), vec2(-klhgG.x, klhgG.x)));
    mediump vec2 bothB = max(max(vec2(-ijfeB.z, ijfeB.z), vec2(-klhgB.w, klhgB.w)), max(vec2(-ijfeB.y, ijfeB.y), vec2(-klhgB.x, klhgB.x)));
    mediump vec2 pR = vec2(0.0), pG = vec2(0.0), pB = vec2(0.0), pW = vec2(0.0);
    easuTapH(pR, pG, pB, pW, vec2(0.0, 1.0) - ppp.xx, vec2(-1.0, -1.0) - ppp.yy, dir, len2, lob, clp, bczzR.xy, bczzG.xy, bczzB.xy);   // b c
    easuTapH(pR, pG, pB, pW, vec2(-1.0, 0.0) - ppp.xx, vec2(1.0, 1.0) - ppp.yy, dir, len2, lob, clp, ijfeR.xy, ijfeG.xy, ijfeB.xy);    // i j
    easuTapH(pR, pG, pB, pW, vec2(0.0, -1.0) - ppp.xx, vec2(0.0, 0.0) - ppp.yy, dir, len2, lob, clp, ijfeR.zw, ijfeG.zw, ijfeB.zw);    // f e
    easuTapH(pR, pG, pB, pW, vec2(1.0, 2.0) - ppp.xx, vec2(1.0, 1.0) - ppp.yy, dir, len2, lob, clp, klhgR.xy, klhgG.xy, klhgB.xy);     // k l
    easuTapH(pR, pG, pB, pW, vec2(2.0, 1.0) - ppp.xx, vec2(0.0, 0.0) - ppp.yy, dir, len2, lob, clp, klhgR.zw, klhgG.zw, klhgB.zw);     // h g
    easuTapH(pR, pG, pB, pW, vec2(1.0, 0.0) - ppp.xx, vec2(2.0, 2.0) - ppp.yy, dir, len2, lob, clp, zzonR.zw, zzonG.zw, zzonB.zw);     // o n
    mediump vec3 aC = vec3(pR.x + pR.y, pG.x + pG.y, pB.x + pB.y);
    mediump float aW = pW.x + pW.y;
    return vec4(min(vec3(bothR.y, bothG.y, bothB.y), max(-vec3(bothR.x, bothG.x, bothB.x), aC * (1.0 / aW))), 1.0);
}
#endif

void main()
{
    vec2 ratio = vec2(dFdx(TexCoord0.x), dFdy(TexCoord0.y)) * vec2(textureSize(tSrc, 0));   // uniform control flow: before any branch
    [[dont_flatten]] if (abs(ratio.x - 1.0) < (1.0 / 1024.0) || !(ratio.x > 0.0) || ratio.x > 1.0) outColor = fxaa();
#ifdef EASU_H
    else outColor = easuH(vec2(ratio.x, ratio.y > 0.0 && ratio.y <= 1.0 ? ratio.y : ratio.x));
#else
    else outColor = easu(vec2(ratio.x, ratio.y > 0.0 && ratio.y <= 1.0 ? ratio.y : ratio.x));
#endif
}
