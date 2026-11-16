#version 450
#extension GL_EXT_control_flow_attributes : require
#extension GL_GOOGLE_include_directive : require
// DOS2 ClusteredLightingWithSSAO (EngineShaders:Shaders/NX/ClusteredLightingWithSSAO.bshd, fragment key d236b8d5b7ffad3c): full frame,
// the deferred lighting of the G-buffer (sun + clustered point / spot lights) on top of the DeferredSky IBL result.
// Derived from the translated SPIR-V (spirv-opt -O); names are ours:
//   g0 = t_E (x roughness, y reflectance, z metal mask, w = (FXEmissive + 2 * ShadingModel) / 255), n = t_A.xy (stereographic normal),
//   d = t_12.x (linear depth), em = t_8.rgb (emissive), base = t_C.rgb, aniso = t_10 (octahedral tangent in x / y / z, anisotropy in w;
//   only for shading model != 0), e = t_32(0, 0).x (exposure), amb = t_14.rgb (DeferredSky output, added at the end), shadow = t_18.x
//   (the "Light Shadows" CascadedPCF result, when c4[0].w > 0).
//   Unlit (model 1) or sky (d == 1): out = (fma(max(0, E), e, U), 0) with E = FXEmissive ? 0 : em, U = FXEmissive && c16[7].y == 0 ? em : 0.
//   Lit: N = c4 rows 1..3 * decoded normal, V = d * ray, world = fma(d, ray, c15[12]), cluster = (slice * c15[14].w + y / 128) *
//   c15[14].z + x / 128, slice = clamp(fma(log2(d * c15[13].x), c15[14].x, c15[14].y), 0, 31). c12[2 * cluster] = first index into the
//   byte list c11, c12[2 * cluster + 1] = point count | spot count << 16. Light (c13, 48 bytes): pos.xyz, 1 / range; colour.rgb, cos outer;
//   dir.xyz, cos inner. Sun: c16 rows 0..2 .x = direction, .z = colour; reflection vector bent toward the sun disc when R.L < c1[0].z.
//   Model 0: normalised Blinn-Phong (pw = 2 / rough^4), falloff sat(1 - (d^2 / r^2)^2)^2 / max(d^2, eps); model != 0: anisotropic GGX
//   with the decoded tangent. out = (amb + fma(max(0, spec + diff + E), e, U), 1).
// Same operations and fma order as the translated module, so the result is the same float for float. What is gone (no change of value):
//   - the render-scale fix-up of all 9 fetches (fast path when _m5[0..9] are all 1, what the host writes; else the translation's code);
//   - fetches the pixel does not use: t_A / t_C / t_14 / t_18 on unlit or sky pixels, t_10 on model-0 pixels;
//   - per light: the scalar address arithmetic of c11 / c12 / c13 (one vec4 load per 16 bytes instead of shift / mask / load per float),
//     the translation's rcp emulation (x == 0 ? FLT_MAX : x != 1 ? 1 / x : x, which is x == 0 ? FLT_MAX : 1 / x as 1 / 1 is exact), work for lights
//     outside the range / behind the surface (moved under the branches that use it), and the loop-invariant roughness terms
//     (2 / rough^4, the GGX alphas; max(rough, c1[0].y) == rough);
//   - spot lights outside the cone (att == 0): the translation still adds colour * 0 * specular there, i.e. +-0 to sums that end in
//     max(0, x); skipping it gives the same bits for any finite light data (it differs only where the translation's sum is already NaN,
//     an inf / NaN light colour);
//   - the dead 1 / (1 + |n|^2) of the emissive select.
// Interface = the translated bundle's: 4 support, 64 c12, 70 c11, 76 c13, 82 c16, 88 c1, 94 c4, 100 c15, 10 t_E, 16 t_A, 22 t_12, 28 t_8,
// 34 t_C, 40 t_10, 46 t_32, 52 t_14, 58 t_18; in location 2 noperspective (view ray); out 0.
#define NEED_RS
#include "common.glsl"
SUPPORT_BUFFER(4)
layout(set = 0, binding = 64, std140) uniform fp_c12 { uvec4 v[4096]; } c12;
layout(set = 0, binding = 70, std140) uniform fp_c11 { uvec4 v[4096]; } c11;
CBUF(76, c13)
CBUF(82, c16)
CBUF(88, c1)
CBUF(94, c4)
CBUF(100, c15)
layout(set = 0, binding = 10) uniform sampler2D tG0;
layout(set = 0, binding = 16) uniform sampler2D tNormal;
layout(set = 0, binding = 22) uniform sampler2D tDepth;
layout(set = 0, binding = 28) uniform sampler2D tEmissive;
layout(set = 0, binding = 34) uniform sampler2D tBase;
layout(set = 0, binding = 40) uniform sampler2D tAniso;
layout(set = 0, binding = 46) uniform sampler2D tExposure;
layout(set = 0, binding = 52) uniform sampler2D tAmbient;
layout(set = 0, binding = 58) uniform sampler2D tShadow;
layout(location = 2) noperspective in vec4 Ray;
layout(location = 0) out vec4 outColor;

const float FMAX = 3.40282347e+38;
// the translation's reciprocal: x == 0 ? FLT_MAX : (x != 1 ? 1 / x : x); 1 / 1 == 1 exactly, so the middle test is dropped
float rcpz(float x) { return x == 0.0 ? FMAX : 1.0 / x; }
// the GGX normalisation 1 / (pi * p): here the x == 1 case is not 1 / x, so it stays
float rcppi(float p)
{
    float q = p * 3.14159274;
    return q == 0.0 ? FMAX : (q != 1.0 ? 0.318309873 / p : q);
}
uint light_index(uint p) { return (c11.v[p >> 4][(p >> 2) & 3u] >> ((p & 3u) << 3)) & 255u; }
float blinn(float pw, float nx, float ny, float nz, vec3 h, float ih, float eps)
{
    return (pw * exp2((pw + -2.0) * log2(max(clamp(fma(nz, h.z * ih, fma(ny, h.y * ih, nx * (h.x * ih))), 0.0, 1.0), eps)))) * 0.159154937;
}

void main()
{
    ivec2 c, pE, pA, pD, pM, pC, pT, pX, pH, pS;
    [[dont_flatten]] if (sb._m5[0] == 1.0 && sb._m5[1] == 1.0 && sb._m5[2] == 1.0 && sb._m5[3] == 1.0 && sb._m5[4] == 1.0 &&
                         sb._m5[5] == 1.0 && sb._m5[6] == 1.0 && sb._m5[7] == 1.0 && sb._m5[8] == 1.0 && sb._m5[9] == 1.0) {
        c = ivec2(gl_FragCoord.xy);
        pE = c; pA = c; pD = c; pM = c; pC = c; pT = c; pH = c; pS = c; pX = ivec2(0);
    } else {
        float s0 = sb._m5[0];
        c = ivec2(int(trunc(gl_FragCoord.x / s0)), int(trunc(gl_FragCoord.y / s0)));
        pE = rs_fix2(c, sb._m5[1]); pA = rs_fix2(c, sb._m5[2]); pD = rs_fix2(c, sb._m5[3]); pM = rs_fix2(c, sb._m5[4]);
        pC = rs_fix2(c, sb._m5[5]); pT = rs_fix2(c, sb._m5[6]); pH = rs_fix2(c, sb._m5[8]); pS = rs_fix2(c, sb._m5[9]);
        float s7 = sb._m5[7];
        pX = s7 < 0.0 ? ivec2(int(mod(gl_FragCoord.x, -s7)), int(mod(gl_FragCoord.y, -s7))) : ivec2(0);
    }
    vec4 g0 = texelFetch(tG0, pE, 0);
    float d = texelFetch(tDepth, pD, 0).x;
    vec3 em = texelFetch(tEmissive, pM, 0).xyz;
    int flags = int(uint(max(roundEven(g0.w * 255.0), 0.0)));
    uint fx = uint(flags & 1);
    int model = int(uint(flags & 6) >> 1);
    bool emis = fx > 0u && 0.0 == c16._m0[7].y;
    vec3 E = fx > 0u ? vec3(0.0) : em;
    vec3 U = emis ? em : vec3(0.0);
    float e = texelFetch(tExposure, pX, 0).x;
    float ce = e == 0.0 ? 0.00999999978 : e;
    [[dont_flatten]] if (model == 1 || d == 1.0) {
        outColor = vec4(fma(max(vec3(0.0), E), vec3(ce), U), 0.0);
        return;
    }
    vec2 n = texelFetch(tNormal, pA, 0).xy;
    vec3 base = texelFetch(tBase, pC, 0).xyz;
    vec3 amb = texelFetch(tAmbient, pH, 0).xyz;
    float sh = 1.0;
    [[dont_flatten]] if (0.0 < c4._m0[0].w) sh = texelFetch(tShadow, pS, 0).x;
    vec4 k1 = c1._m0[0], k2 = c1._m0[1];
    float ex = fma(n.x, k1.x, -1.77769995), ey = fma(n.y, k1.x, -1.77769995);
    float s = 2.0 / (fma(ey, ey, ex * ex) + 1.0);
    float nx0 = ex * s, ny0 = ey * s, nz0 = s + -1.0;
    vec4 r1 = c4._m0[1], r2 = c4._m0[2], r3 = c4._m0[3];
    float Nx = fma(nz0, r1.z, fma(nx0, r1.x, ny0 * r1.y));
    float Ny = fma(nz0, r2.z, fma(nx0, r2.x, ny0 * r2.y));
    float Nz = fma(nz0, r3.z, fma(nx0, r3.x, ny0 * r3.y));
    float m2 = g0.y * g0.y;
    float rough = max(g0.x, k1.y);
    float rr = rough * rough;
    vec3 V = d * Ray.xyz;
    vec3 Wp = fma(vec3(d), Ray.xyz, c15._m0[12].xyz);
    vec4 cc = c15._m0[14];
    int slice = int(trunc(max(0.0, min(fma(log2(d * c15._m0[13].x), cc.x, cc.y), 31.0))));
    int cluster = (slice * int(trunc(cc.w)) + (c.y >> 7)) * int(trunc(cc.z)) + (c.x >> 7);
    float iv = -inversesqrt(fma(V.z, V.z, fma(V.y, V.y, V.x * V.x)));
    vec3 Vn = V * iv;
    float eps = k1.w, minl = k2.x, one = k2.y, kd = k2.z, ceps = k2.w;
    vec4 s0 = c16._m0[0], s1 = c16._m0[1], s2 = c16._m0[2];
    float Lx = s0.x, Ly = s1.x, Lz = s2.x;
    vec3 sc = vec3(s0.z, s1.z, s2.z);
    vec3 dc = fma(vec3(g0.z), -base, base);
    float ndv = fma(Nz, Vn.z, fma(Ny, Vn.y, Nx * Vn.x));
    float ndl = clamp(fma(Nz, Lz, fma(Ny, Ly, Nx * Lx)), 0.0, 1.0);
    float Rx = fma(Nx * ndv, -2.0, Vn.x), Ry = fma(Ny * ndv, -2.0, Vn.y), Rz = fma(Nz * ndv, -2.0, Vn.z);
    float rdl = fma(Rz, Lz, fma(Ry, Ly, Rx * Lx));
    vec3 Rp = vec3(Rx, Ry, Rz);
    [[dont_flatten]] if (rdl < k1.z) {
        float Tx = fma(rdl, -Lx, Rx), Ty = fma(rdl, -Ly, Ry), Tz = fma(rdl, -Lz, Rz);
        float it = inversesqrt(fma(Tz, Tz, fma(Ty, Ty, Tx * Tx)));
        float Bx = fma(0.99998945, Lx, (Tx * it) * 0.00459006475);
        float By = fma(0.99998945, Ly, (Ty * it) * 0.00459006475);
        float Bz = fma(0.99998945, Lz, (Tz * it) * 0.00459006475);
        float ib = inversesqrt(fma(Bz, Bz, fma(By, By, Bx * Bx)));
        Rp = vec3(Bx * ib, By * ib, Bz * ib);
    }
    vec3 H = fma(V, vec3(iv), Rp);
    float f0 = fma(g0.z, m2 * -0.159999996, m2 * 0.159999996);
    vec3 F = fma(vec3(g0.z), base, vec3(f0));
    uint w = uint(cluster << 3) >> 2;
    uvec4 q4 = c12.v[w >> 2];
    uvec2 oc = (w & 2u) != 0u ? q4.zw : q4.xy;
    uint p = oc.x, np = oc.y & 65535u, ns = uint(int(oc.y) >> 16);
    vec3 spec, diff;
    [[dont_flatten]] if (model == 0) {
        float pw = 2.0 / (rr * rr);
        float ih = inversesqrt(fma(H.z, H.z, fma(H.y, H.y, H.x * H.x)));
        float D = blinn(pw, Nx, Ny, Nz, H, ih, eps);
        diff = (ndl * ((dc * 0.318309873) * sc)) * sh;
        spec = ((ndl * ((F * D) * sc)) * 0.25) * sh;
        for (uint i = 0u; i < np; i++, p++) {
            uint li = light_index(p) * 3u;
            vec4 A = c13._m0[li];
            vec3 L = -Wp + A.xyz;
            float rq = A.w * A.w;
            float dd = fma(L.z, L.z, fma(L.y, L.y, L.x * L.x));
            vec3 Ln = L * inversesqrt(max(dd, minl));
            [[dont_flatten]] if (dd < rcpz(rq)) {
                float nl = clamp(fma(Nz, Ln.z, fma(Ny, Ln.y, Nx * Ln.x)), 0.0, 1.0);
                [[dont_flatten]] if (nl > 0.0) {
                    vec3 Hl = fma(V, vec3(iv), Ln);
                    float ihh = inversesqrt(max(fma(Hl.z, Hl.z, fma(Hl.y, Hl.y, Hl.x * Hl.x)), minl));
                    float qq = dd * rq;
                    float fall = clamp(fma(qq, -qq, one), 0.0, 1.0);
                    float a = (nl * ((fall * fall) * rcpz(max(dd, eps)))) * 0.0795774683;
                    vec3 col = c13._m0[li + 1u].xyz * a;
                    float Dl = blinn(pw, Nx, Ny, Nz, Hl, ihh, eps);
                    spec = fma(col * (F * Dl), vec3(0.25), spec);
                    diff = fma(dc * col, vec3(kd), diff);
                }
            }
        }
        for (uint i = 0u; i < ns; i++, p++) {
            uint li = light_index(p) * 3u;
            vec4 A = c13._m0[li];
            vec3 L = -Wp + A.xyz;
            float rq = A.w * A.w;
            float dd = fma(L.z, L.z, fma(L.y, L.y, L.x * L.x));
            vec3 Ln = L * inversesqrt(max(dd, minl));
            [[dont_flatten]] if (dd < rcpz(rq)) {
                vec4 B = c13._m0[li + 1u], C = c13._m0[li + 2u];
                float idv = inversesqrt(fma(C.z, C.z, fma(C.y, C.y, C.x * C.x)));
                float cs = fma(Ln.z, -(C.z * idv), fma(Ln.y, -(C.y * idv), Ln.x * -(C.x * idv)));
                float nl = clamp(fma(Nz, Ln.z, fma(Ny, Ln.y, Nx * Ln.x)), 0.0, 1.0);
                float att = 0.0;
                [[dont_flatten]] if (cs > B.w) {
                    float qq = dd * rq;
                    float fall = clamp(fma(qq, -qq, one), 0.0, 1.0);
                    float cone = clamp((-B.w + cs) * (1.0 / max(C.w + -B.w, ceps)), 0.0, 1.0);
                    att = nl * ((cone * cone) * ((fall * fall) * rcpz(max(dd, eps))));
                }
                [[dont_flatten]] if (nl > 0.0 && att != 0.0) {
                    vec3 Hl = fma(V, vec3(iv), Ln);
                    float ihh = inversesqrt(max(fma(Hl.z, Hl.z, fma(Hl.y, Hl.y, Hl.x * Hl.x)), minl));
                    vec3 col = B.xyz * (att * 0.318309873);
                    float Dl = blinn(pw, Nx, Ny, Nz, Hl, ihh, eps);
                    spec = fma(col * (F * Dl), vec3(0.25), spec);
                    diff = fma(dc * col, vec3(kd), diff);
                }
            }
        }
    } else {
        vec4 t10 = texelFetch(tAniso, pT, 0);
        float fl = floor(t10.y * 15.9375);
        vec4 k3 = c1._m0[2];
        float ox = max(min(fma(fma(t10.x, 4080.0, fl), k3.x, -1.0), 1.0), -1.0);
        float oy = max(min(fma(fma(fma(t10.y, 15.9375, -fl), 4096.0, t10.z * 255.0), k3.x, -1.0), 1.0), -1.0);
        float ax = -abs(ox), ay = -abs(oy), sm = ax + ay;
        float tx = ox, ty = oy;
        [[dont_flatten]] if (!(sm >= -1.0)) {
            float sy = !(oy >= 0.0) ? -1.0 : 1.0, sx = !(ox >= 0.0) ? -1.0 : 1.0;
            ty = fma(sy, ax, sy); tx = fma(sx, ay, sx);
        }
        float tz = sm + 1.0;
        float itn = inversesqrt(fma(tz, tz, fma(ty, ty, tx * tx)));
        float Ty = itn * ty, Tx = itn * tx, Tz = tz * itn;
        float TX = fma(Tz, r1.z, fma(Tx, r1.x, Ty * r1.y));
        float TY = fma(Tz, r2.z, fma(Tx, r2.x, Ty * r2.y));
        float TZ = fma(Tz, r3.z, fma(Tx, r3.x, Ty * r3.y));
        float an = fma(t10.w, k3.y, 1.0);
        float ia = inversesqrt(an), sa = sqrt(an);
        float mn = k3.z;
        float at = max(rr * sa, mn), ab = max(clamp(rr * ia, 0.0, 1.0), mn);
        float iat2 = 1.0 / (at * at), iab2 = 1.0 / (ab * ab);
        float ih = inversesqrt(fma(H.z, H.z, fma(H.y, H.y, H.x * H.x)));
        vec3 Hn = H * ih;
        float tdh = fma(TZ, Hn.z, fma(TY, Hn.y, TX * Hn.x));
        float ndh = clamp(fma(Nz, Hn.z, fma(Ny, Hn.y, Nx * Hn.x)), 0.0, 1.0);
        float tdh2 = tdh * tdh;
        float dn = fma(ndh, ndh, fma(tdh2, iab2, fma(fma(ndh, -ndh, -tdh2), iat2, iat2)));
        float Dv = rcppi(dn * ((at * ab) * dn));
        diff = (ndl * ((dc * 0.318309873) * sc)) * sh;
        spec = ((ndl * ((F * Dv) * sc)) * 0.25) * sh;
        for (uint i = 0u; i < np; i++, p++) {
            uint li = light_index(p) * 3u;
            vec4 A = c13._m0[li];
            vec3 L = -Wp + A.xyz;
            float rq = A.w * A.w;
            float dd = fma(L.z, L.z, fma(L.y, L.y, L.x * L.x));
            vec3 Ln = L * inversesqrt(max(dd, minl));
            [[dont_flatten]] if (dd < rcpz(rq)) {
                float nl = clamp(fma(Nz, Ln.z, fma(Ny, Ln.y, Nx * Ln.x)), 0.0, 1.0);
                [[dont_flatten]] if (nl > 0.0) {
                    vec3 Hl = fma(V, vec3(iv), Ln);
                    float ihh = inversesqrt(max(fma(Hl.z, Hl.z, fma(Hl.y, Hl.y, Hl.x * Hl.x)), minl));
                    vec3 hn = Hl * ihh;
                    float ndhl = clamp(fma(Nz, hn.z, fma(Ny, hn.y, Nx * hn.x)), 0.0, 1.0);
                    float tdhl = fma(TZ, hn.z, fma(TY, hn.y, TX * hn.x));
                    float qq = dd * rq;
                    float fall = clamp(fma(qq, -qq, one), 0.0, 1.0);
                    float a = (nl * ((fall * fall) * rcpz(max(dd, eps)))) * 0.0795774683;
                    float t2 = tdhl * tdhl;
                    float dl = fma(ndhl, ndhl, fma(t2, iab2, fma(iat2, fma(ndhl, -ndhl, -t2), iat2)));
                    float Dl = rcppi(dl * (dl * (ab * at)));
                    vec3 col = c13._m0[li + 1u].xyz * a;
                    spec = fma(col * (F * Dl), vec3(0.25), spec);
                    diff = fma(dc * col, vec3(kd), diff);
                }
            }
        }
        for (uint i = 0u; i < ns; i++, p++) {
            uint li = light_index(p) * 3u;
            vec4 A = c13._m0[li];
            vec3 L = -Wp + A.xyz;
            float rq = A.w * A.w;
            float dd = fma(L.z, L.z, fma(L.y, L.y, L.x * L.x));
            vec3 Ln = L * inversesqrt(max(dd, minl));
            [[dont_flatten]] if (dd < rcpz(rq)) {
                vec4 B = c13._m0[li + 1u], C = c13._m0[li + 2u];
                float idv = inversesqrt(fma(C.z, C.z, fma(C.y, C.y, C.x * C.x)));
                float nl = clamp(fma(Nz, Ln.z, fma(Ny, Ln.y, Nx * Ln.x)), 0.0, 1.0);
                float cs = fma(Ln.z, -(C.z * idv), fma(Ln.y, -(C.y * idv), Ln.x * -(C.x * idv)));
                float att = 0.0;
                [[dont_flatten]] if (cs > B.w) {
                    float qq = dd * rq;
                    float fall = clamp(fma(qq, -qq, one), 0.0, 1.0);
                    float cone = clamp((cs + -B.w) * (1.0 / max(C.w + -B.w, ceps)), 0.0, 1.0);
                    att = nl * ((cone * cone) * ((fall * fall) * rcpz(max(dd, eps))));
                }
                [[dont_flatten]] if (nl > 0.0 && att != 0.0) {
                    vec3 Hl = fma(V, vec3(iv), Ln);
                    float ihh = inversesqrt(max(fma(Hl.z, Hl.z, fma(Hl.y, Hl.y, Hl.x * Hl.x)), minl));
                    vec3 hn = Hl * ihh;
                    float ndhl = clamp(fma(Nz, hn.z, fma(Ny, hn.y, Nx * hn.x)), 0.0, 1.0);
                    float tdhl = fma(TZ, hn.z, fma(TY, hn.y, TX * hn.x));
                    float t2 = tdhl * tdhl;
                    float dl = fma(ndhl, ndhl, fma(iab2, t2, fma(iat2, fma(ndhl, -ndhl, -t2), iat2)));
                    float Dl = rcppi(dl * ((ab * at) * dl));
                    vec3 col = B.xyz * (att * 0.318309873);
                    spec = fma(col * (F * Dl), vec3(0.25), spec);
                    diff = fma(dc * col, vec3(kd), diff);
                }
            }
        }
    }
    outColor = vec4(amb + fma(max(vec3(0.0), (spec + diff) + E), vec3(ce), U), 1.0);
}
