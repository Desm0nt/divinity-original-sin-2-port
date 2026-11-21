"""Divinity: Original Sin 2 DE, N6 DM_HACK_LIGHTS (docs/dos2/nr/NR-HACKS.md): build and prove the light-cap variants of the lighting pass.

tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag (the NR-04 'x' rewrite, proven bit for bit against the translated module by
tools/dos2_light_mobile.py) compiled with -DLCAP=2 / 4 -> dm/android/shaders/light_lcap<N>.frag.spv.inc (the backend wraps them with the
player's translated head, xbg_nr_sh.inc hack_apply). Proofs with tools/spv_interp.py on tools/dos2_light_mobile.py's cases:
  1. LCAP=99 (never reached) == the 'x' module bit for bit (the changed loops alone change nothing);
  2. LCAP=N == the 'x' module run on a prefix of the cluster's list (points, then spots; the list rewritten), bit for bit - the prefix that
     ends at the N-th light that lights the pixel: the variant is exactly "the first N contributing lights";
  3. cost: executed instructions per lit pixel for 0..8 points / 0..4 spots (the 'x' module), the per-light increment.
[gpu2] DM_VK_NATIVE_LCAP_SEL (vkn_hacks.h): 1 (default) = the per-light threshold of thr_source (light_lcap<N>t.frag.spv.inc; the
poison twin light_lcap<N>tp), proofs prove_thr / sweep / cost_strong; the strongest-N cap (strong_source) is measured and rejected
(--only strong: it costs more than no cap). The first-N modules above stay for DM_VK_NATIVE_LCAP_SEL=0.
Usage: dos2_light_cap.py [--bundles work/shaders-vk] [--n 200] [--only old|thr|strong]
"""
import argparse, struct, tempfile, sys
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from dos2_native_shaders import read_bundle, tool, WORK
from dos2_pcf_mobile import run, check_iface
import spv_interp as SI
import dos2_light_mobile as LM

KEY = 'd236b8d5b7ffad3c'
SRC = HERE / 'dos2_post' / 'ClusteredLightingWithSSAO_mobile.frag'
OUT = HERE.parent / 'dm' / 'android' / 'shaders'


CAP_AT = '    uint p = oc.x, np = oc.y & 65535u, ns = uint(int(oc.y) >> 16);\n'
CAP_ADD = ('#ifdef LCAP\n'
           '    // [DOS2] N6 DM_HACK_LIGHTS: the first LCAP lights that light this pixel (in range, facing it; points, then spots, in the list\'s\n'
           '    // order) - after the LCAP-th the loops stop; lights out of range cost only their range test, as before\n'
           '    const uint p0 = p, npo = np;\n'
           '    uint lit = 0u;\n'
           '#endif\n')
LOOP_P = '        for (uint i = 0u; i < np; i++, p++) {\n'
LOOP_S = '        for (uint i = 0u; i < ns; i++, p++) {\n'
SPOT_ADD = '#ifdef LCAP\n        p = p0 + npo;   // the spots start after all the cluster\'s points (the point loop may stop early)\n#endif\n'
HIT = '                    diff = fma(dc * col, vec3(kd), diff);\n'


def capped_source():
    """the 'x' rewrite with the cap inserted (text patch at build time: the *_mobile.frag files feed setup's NR-04 pack stamp, so the
    source itself stays untouched): a counter of contributing lights, the loops stop at LCAP, the spot loop restarts at its own list"""
    s = SRC.read_text(encoding='utf-8')
    assert s.count(CAP_AT) == 1 and s.count(LOOP_P) == 2 and s.count(LOOP_S) == 2 and s.count(HIT) == 4, \
        'ClusteredLightingWithSSAO_mobile.frag changed: update the LCAP anchors'
    s = s.replace(CAP_AT, CAP_AT + CAP_ADD, 1)
    s = s.replace(LOOP_P, '#ifdef LCAP\n        for (uint i = 0u; i < np && lit < uint(LCAP); i++, p++) {\n#else\n' + LOOP_P + '#endif\n')
    s = s.replace(LOOP_S, SPOT_ADD + '#ifdef LCAP\n        for (uint i = 0u; i < ns && lit < uint(LCAP); i++, p++) {\n#else\n' + LOOP_S + '#endif\n')
    s = s.replace(HIT, HIT + '#ifdef LCAP\n                    lit++;\n#endif\n')
    return s


def compile_(defines, tr):
    with tempfile.TemporaryDirectory() as tmp:
        spv = Path(tmp) / 'l.spv'; src = Path(tmp) / 'ClusteredLightingWithSSAO_lcap.frag'
        src.write_text(capped_source(), encoding='utf-8')
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', *defines, '-I', str(SRC.parent), '-o', str(spv), str(src)])
        run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
        data = spv.read_bytes()
    check_iface(data, tr['spirv'])
    return data


def write_inc(data, path, what):
    w = struct.unpack('<%dI' % (len(data) // 4), data)
    lines = ['// [DOS2] N6 DM_HACK_LIGHTS: SPIR-V of tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag + the LCAP text patch of tools/dos2_light_cap.py %s (glslc --target-env=vulkan1.1 -O), '
             'made by tools/dos2_light_cap.py' % what]
    for i in range(0, len(w), 8): lines.append(', '.join('0x%08x' % x for x in w[i:i + 8]) + ',')
    path.write_text('\n'.join(lines) + '\n')


def shared_list(rng, np_, ns_):
    """c12 with every cluster at offset 0 and the same counts, c11 = random light indices: any pixel reads the same list"""
    n = 16384 // 2
    c12 = np.stack([np.zeros(n, np.uint32), np.full(n, np_ | (ns_ << 16), np.uint32)], 1).astype(np.uint32).tobytes()
    c11 = bytearray(rng.integers(0, LM.NL, 65536).astype(np.uint8).tobytes())
    return c12, c11


def prefix(c11, np_, ns_, k):
    """the cluster cut to the first k entries of (points, then spots): the list rewritten, counts lowered"""
    kp = min(np_, k); ks = min(ns_, k - kp)
    b = bytearray(c11); b[0:kp] = c11[0:kp]; b[kp:kp + ks] = c11[np_:np_ + ks]
    n = 16384 // 2
    c12 = np.stack([np.zeros(n, np.uint32), np.full(n, kp | (ks << 16), np.uint32)], 1).astype(np.uint32).tobytes()
    return c12, bytes(b)


def prove(x, var, big, cap, n, seed):
    """1. LCAP=99 (never reached) == x bit for bit: the loop change itself changes nothing;
    2. LCAP=cap == x on some prefix of the list (the prefix that ends at the cap-th contributing light), bit for bit; also counted: how often
       the cap cut anything (the variant differs from x on the whole list)"""
    rng = np.random.default_rng(seed)
    mx, mv, mb = SI.Module(x), SI.Module(var), SI.Module(big)
    same_big = pref = cut = 0
    for it in range(n):
        np_, ns_ = int(rng.integers(0, 13)), int(rng.integers(0, 5))
        model = int(rng.choice([0, 0, 2]))
        inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, it % 4 == 3, (np_, ns_, model))
        c12, c11 = shared_list(rng, np_, ns_); ub[64], ub[70] = c12, bytes(c11)
        full = mx.run(inp, ub, tx); v = mv.run(inp, ub, tx)
        same_big += LM.same(mb.run(inp, ub, tx), full)
        if not LM.same(v, full): cut += 1
        ok = False
        for k in range(np_ + ns_ + 1):
            ub2 = dict(ub); ub2[64], ub2[70] = prefix(c11, np_, ns_, k)
            if LM.same(mx.run(inp, ub2, tx), v): ok = True; break
        pref += ok
    print('LCAP=%d: LCAP=99 == x bit for bit %d / %d; variant == x on a prefix of the list %d / %d (the cap cut something in %d)'
          % (cap, same_big, n, pref, n, cut))
    return (n - same_big) + (n - pref)


def cost(x, n=40):
    rng = np.random.default_rng(3); m = SI.Module(x); rows = []
    for np_, ns_ in ((0, 0), (1, 0), (2, 0), (4, 0), (8, 0), (0, 1), (0, 2), (0, 4), (4, 2), (8, 4)):
        ex = []
        for it in range(n):
            inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, False, (np_, ns_, 0))
            c12, c11 = shared_list(rng, np_, ns_); ub[64], ub[70] = c12, bytes(c11)
            m.run(inp, ub, tx); ex.append(m.executed)
        rows.append((np_, ns_, float(np.mean(ex))))
    base = rows[0][2]
    for np_, ns_, e in rows: print('  cost x module: %d point + %d spot in the cluster: %.0f instructions / lit pixel (+%.0f over no lights)' % (np_, ns_, e, e - base))
    return rows


# ---------------------------------------------------------------------------------------------------------------------------------------
# [DOS2] gpu2 (VK-MOBILE-GPU-PLAN.md section 8, DM_VK_NATIVE_LCAP_SEL=1): the cap keeps the LCAP STRONGEST lights of the pixel instead of the
# first LCAP of the engine's list. The owner saw lights flicker in the shadows at hack tiers 2 / 3 (runs S04 / S05): the engine's cluster
# list order changes from frame to frame, so "the first N" is another set each frame, and at a 128 px cluster border another one again.
# Here: pass 1 walks the whole list once with the cheap part of every light (range, facing, falloff, cone: the contribution's size
# without the BRDF lobe) and keeps the LCAP largest weights w = lum(|colour|) x attenuation at the pixel; pass 2 draws only those, in list
# order (the sum order of the uncapped module), each with the uncapped module's own code. The weakest kept light fades out against the
# strongest dropped one (fade = sat((w_last - w_cut) / (LFADE w_last)), LFADE 0.25): where the set changes (w_last == w_cut) that light is
# at 0, so the picture is continuous in the lights' positions and colours - no light pops on or off - and the result depends on the SET of
# lights only, not on their list order (ties at the border fade to 0 on both sides). Nothing dropped -> fade 1 and pass 2 = the uncapped
# module bit for bit (the proofs below). Cost: pass 1 is about half a light's work for every light in range; see cost_strong().
LFADE = 0.25
LUMA = 'vec3(0.2126, 0.7152, 0.0722)'


def _loops(s, hdr):
    """[(start, end, body)] of the loops whose header line is hdr (the body = the lines inside the braces)"""
    out, i = [], 0
    while True:
        i = s.find(hdr, i)
        if i < 0: return out
        e = s.index('\n        }\n', i)
        out.append((i, e + len('\n        }\n'), s[i + len(hdr):e + 1])); i = e


def _insert(n):
    """the insertion of (lw, i) into the descending top-n slots lw0.., lk0.. for a light that enters (lw > lw<n-1>); lcut = the largest
    weight that left (an empty slot holds 0: a weight of 0 never enters)"""
    b = ['bool b%d = lw > lw%d;' % (k, k) for k in range(n - 1)]
    lines = ([' '.join(b)] if b else []) + ['lcut = max(lcut, lw%d);' % (n - 1)]
    for k in range(n - 1, 0, -1):
        hi = 'lw' if k == n - 1 else '(b%d ? lw : lw%d)' % (k, k)
        hk = 'i' if k == n - 1 else '(b%d ? i : lk%d)' % (k, k)
        lines.append('lw%d = b%d ? lw%d : %s; lk%d = b%d ? lk%d : %s;' % (k, k - 1, k - 1, hi, k, k - 1, k - 1, hk))
    lines.append('lw0 = b0 ? lw : lw0; lk0 = b0 ? i : lk0;' if n > 1 else 'lw0 = lw; lk0 = i;')
    return lines


def _sort(n):
    """a sorting network over ls0..ls(n-1) (ascending list positions; LNONE sorts last)"""
    net = {1: [], 2: [(0, 1)], 3: [(0, 1), (1, 2), (0, 1)], 4: [(0, 1), (2, 3), (0, 2), (1, 3), (1, 2)]}[n]
    return ['{ uint t = min(ls%d, ls%d); ls%d = max(ls%d, ls%d); ls%d = t; }' % (a, b, b, a, b, a) for a, b in net]


COLP, COLS = 'vec3 col = c13._m0[li + 1u].xyz * a;', 'vec3 col = B.xyz * (att * 0.318309873);'


def strong_source(cap, poison=False, probe=False):
    """the 'x' rewrite with the strongest-N cap (text patch at build time, as capped_source; the source file stays untouched)"""
    s = SRC.read_text(encoding='utf-8')
    assert s.count(CAP_AT) == 1 and s.count(LOOP_P) == 2 and s.count(LOOP_S) == 2, 'ClusteredLightingWithSSAO_mobile.frag changed: update the LCAP anchors'
    n = int(cap); assert 1 <= n <= 4
    p1 = ['#ifndef LFADE', '#define LFADE %r' % LFADE, '#endif',
          '    // [DOS2] gpu2 DM_VK_NATIVE_LCAP_SEL=1 (tools/dos2_light_cap.py strong_source): the %d STRONGEST lights of this pixel, the weakest' % n,
          '    // kept one faded against the strongest dropped one; pass 1 = the size of each contribution without the BRDF lobe',
          '    const uint LNONE = 0xffffffffu;', '    const uint p0 = p, npo = np;',
          '    float lcut = 0.0, ' + ', '.join('lw%d = 0.0' % k for k in range(n)) + ';',
          '    uint ' + ', '.join('lk%d = LNONE' % k for k in range(n)) + ';',
          '    const bool lsel = np + ns > %du;   // a list no longer than the cap: the uncapped loops (nothing to choose; exact)' % n,
          '    [[dont_flatten]] if (lsel) for (uint i = 0u; i < np + ns; i++) {',
          '        uint li = light_index(p0 + i) * 3u;',
          '        vec4 A = c13._m0[li];',
          '        vec3 L = -Wp + A.xyz;',
          '        float rq = A.w * A.w;',
          '        float dd = fma(L.z, L.z, fma(L.y, L.y, L.x * L.x));',
          '        [[dont_flatten]] if (dd < rcpz(rq)) {',
          '            vec3 Ln = L * inversesqrt(max(dd, minl));',
          '            float nl = clamp(fma(Nz, Ln.z, fma(Ny, Ln.y, Nx * Ln.x)), 0.0, 1.0);',
          '            vec4 B = c13._m0[li + 1u];',
          '            float qq = dd * rq;',
          '            float fall = clamp(fma(qq, -qq, one), 0.0, 1.0);',
          '            float at = 0.0;',
          '            [[dont_flatten]] if (i < npo) {',
          '                at = (nl * ((fall * fall) * rcpz(max(dd, eps)))) * 0.0795774683;',
          '            } else {',
          '                vec4 C = c13._m0[li + 2u];',
          '                float idv = inversesqrt(fma(C.z, C.z, fma(C.y, C.y, C.x * C.x)));',
          '                float cs = fma(Ln.z, -(C.z * idv), fma(Ln.y, -(C.y * idv), Ln.x * -(C.x * idv)));',
          '                [[dont_flatten]] if (cs > B.w) {',
          '                    float cone = clamp((-B.w + cs) * (1.0 / max(C.w + -B.w, ceps)), 0.0, 1.0);',
          '                    at = (nl * ((cone * cone) * ((fall * fall) * rcpz(max(dd, eps))))) * 0.318309873;',
          '                }',
          '            }',
          '            float lw = at * dot(abs(B.xyz), %s);' % LUMA,
          '            [[dont_flatten]] if (lw > lw%d) {   // it enters (a light the uncapped module adds as +0, lw 0, is no candidate)' % (n - 1)]
    p1 += ['                ' + l for l in _insert(n)]
    p1 += ['            } else lcut = max(lcut, lw);', '        }', '    }',
           '    const uint lkf = lk%d;   // the weakest kept light fades' % (n - 1),
           '    const float lfade = lcut > 0.0 ? clamp((lw%d - lcut) / (float(LFADE) * lw%d), 0.0, 1.0) : 1.0;' % (n - 1, n - 1),
           '    uint ' + ', '.join('ls%d = lk%d' % (k, k) for k in range(n)) + ';']
    p1 += ['    ' + l for l in _sort(n)]
    if probe:   # the interpreter's view of pass 1: kept list positions (8 bits each, 255 = none), the fade, the cut, the faded position
        p1 += ['    uint lpk = 0u;'] + ['    lpk |= (ls%d == LNONE ? 255u : ls%d) << %du;' % (k, k, 8 * k) for k in range(n)] + \
              ['    for (uint k = %du; k < 4u; k++) lpk |= 255u << (8u * k);' % n,
               '    outColor = vec4(uintBitsToFloat(lpk), lfade, lcut, uintBitsToFloat(lkf == LNONE ? 255u : lkf));', '    return;']
    s = s.replace(CAP_AT, CAP_AT + '\n'.join(p1) + '\n', 1)
    sel = 'ls0' if n == 1 else ''.join('k == %du ? ls%d : (' % (k, k) for k in range(n - 1)) + 'ls%d' % (n - 1) + ')' * (n - 1)
    done = 0; pos = 0
    while done < 2:
        lp = [l for l in _loops(s, LOOP_P) if l[0] >= pos][0]; ls_ = [l for l in _loops(s, LOOP_S) if l[0] > lp[0]][0]
        assert s[lp[1]:ls_[0]] == '', 'the point and spot loops are not adjacent'
        bp, bs = lp[2], ls_[2]
        assert bp.count(COLP) == 1 and bs.count(COLS) == 1
        bp = bp.replace(COLP, 'vec3 col = (c13._m0[li + 1u].xyz * lf) * a;'); bs = bs.replace(COLS, 'vec3 col = (B.xyz * lf) * (att * 0.318309873);')
        ind = lambda b: ''.join(('    ' + l if l else l) + '\n' for l in b.rstrip('\n').split('\n'))
        p2 = ('#ifdef LCAP\n'
              '        [[dont_flatten]] if (lsel) for (uint k = 0u; k < %du; k++) {   // pass 2: the kept lights in list order, the uncapped code\n' % n +
              '            uint lq = %s;\n' % sel +
              '            [[dont_flatten]] if (lq == LNONE) break;\n'
              '            float lf = lq == lkf ? lfade : 1.0;   // (x 1.0 is exact: the kept light as the uncapped module draws it)\n'
              '            [[dont_flatten]] if (!(lf > 0.0)) continue;\n'
              '            p = p0 + lq;\n'
              '            [[dont_flatten]] if (lq < npo) {\n' + ind(bp) +
              '            } else {\n' + ind(bs) +
              '            }\n'
              '        } else {\n'
              '#endif\n')
        tail = '#ifdef LCAP\n        }\n#endif\n'
        s = s[:lp[0]] + p2 + s[lp[0]:ls_[1]] + tail + s[ls_[1]:]
        pos = lp[0] + len(p2) + (ls_[1] - lp[0]) + len(tail); done += 1
    if poison:   # [DM_VK_NATIVE_LCAP_SEL=2] where the cap dropped a light: magenta with the pixel's brightness, brighter where it fades one
        FIN = '    outColor = vec4(LF_DF(amb + fma(max(vec3(0.0), (spec + diff) + E), vec3(ce), U)), 1.0);\n'
        assert s.count(FIN) == 1
        s = s.replace(FIN, FIN + '    [[dont_flatten]] if (lcut > 0.0) { float lm = max(max(outColor.r, outColor.g), max(outColor.b, 0.05)) * (2.0 - lfade); outColor = vec4(lm, 0.0, lm, 1.0); }\n')
    return s


def compile_src(text, defines, tr=None):
    with tempfile.TemporaryDirectory() as tmp:
        spv = Path(tmp) / 'l.spv'; src = Path(tmp) / 'ClusteredLightingWithSSAO_lcap.frag'
        src.write_text(text, encoding='utf-8')
        run([tool('glslc'), '--target-env=vulkan1.1', '-O', '-fshader-stage=frag', *defines, '-I', str(SRC.parent), '-o', str(spv), str(src)])
        run([tool('spirv-val'), '--target-env', 'vulkan1.1', str(spv)])
        data = spv.read_bytes()
    if tr is not None: check_iface(data, tr['spirv'])
    return data


def _list(ids, np_):
    n = 16384 // 2; ns_ = len(ids) - np_
    c12 = np.stack([np.zeros(n, np.uint32), np.full(n, np_ | (ns_ << 16), np.uint32)], 1).astype(np.uint32).tobytes()
    c11 = bytearray(65536); c11[0:len(ids)] = bytes(int(i) for i in ids)
    return c12, bytes(c11)


def with_list(ub, ids, np_):
    """ub with the cluster list = ids (light indices of c13; the first np_ points, the rest spots); every cluster reads it"""
    u = dict(ub); u[64], u[70] = _list(ids, np_)
    return u


def wp_of(inp, ub, tx):
    """the pixel's world position as the module computes it (render scale 1: the depth fetch at the fragment's pixel)"""
    x, y = int(inp['FragCoord'][0]), int(inp['FragCoord'][1])
    d = float(tx[22].d[y % tx[22].h, x % tx[22].w][0])
    cam = np.frombuffer(ub[100][16 * 12:16 * 12 + 12], np.float32).astype(np.float64)
    return d * np.array(inp[2][:3], np.float64) + cam


def dense(rng, inp, ub, tx, ids, np_):
    """the lights of the list placed around the pixel's world position (inside 20..80 % of their range; spots aimed at it), so that
    most of them light it - the generator's lights mostly miss a pixel, and a cap needs more contributions than N to cut anything"""
    wp = wp_of(inp, ub, tx)
    c13 = bytearray(ub[76])
    for q, li in enumerate(ids):
        u = LM.unit(rng); r = float(rng.uniform(3, 12)); dist = float(rng.uniform(0.2, 0.8)) * r
        pos = wp + u * dist; col = rng.uniform(0, 4, 3)
        co = float(rng.uniform(0.2, 0.9)); ci = min(co + float(rng.uniform(0.0, 0.2)), 1.0)
        struct.pack_into('<12f', c13, 48 * li, *pos, 1.0 / r, *col, co, *(-u * rng.uniform(0.5, 2)), ci)
    u = dict(ub); u[76] = bytes(c13)
    return u


def probe_of(mp, inp, ub, tx):
    """pass 1 as the module computes it: (kept list positions, fade, cut, faded position) or None (an unlit / sky pixel)"""
    o = mp.run(inp, ub, tx)[0]   # (call only for a lit pixel: see lit())
    b = struct.unpack('<I', struct.pack('<f', o[0]))[0]
    f = struct.unpack('<I', struct.pack('<f', o[3]))[0]
    return [x for x in ((b >> (8 * k)) & 255 for k in range(4)) if x != 255], np.float32(o[1]), np.float32(o[2]), (None if f == 255 else f)


def lit(mx, inp, ub, tx):
    """1 = the pixel takes the lit path (the uncapped module's output alpha is 1; unlit / sky pixels write alpha 0)"""
    o = mx.run(inp, ub, tx)
    return o is not None and o[0][3] == 1.0


def luma(o):
    return float(np.dot(np.array(o[0][:3], np.float64), (0.2126, 0.7152, 0.0722))) if o else 0.0


def prove_strong(x, var, probe, oldv, cap, n, seed):
    """A. nothing dropped (cut 0): the variant == x on the whole list, bit for bit;
    B. something dropped: the variant == x on the kept lights only (list order) with the faded light's colour pre-scaled by the fade
       (B.xyz * fade is what the variant multiplies; the same float), bit for bit - so the variant IS "these lights, this fade";
    C. the list shuffled (points among points, spots among spots: what the engine's reordering does): the kept SET of lights and the fade
       are the same bits, the output equal up to the sum order (relative difference <= 1e-5); for the old cap (the first N) counted how
       often the output changes by more than 1 % (the flicker);
    D. the kept lights are the strongest: every dropped light, put alone in a list with one kept light of its class, loses against it
       (a list of 2 under the cap 1 is not this module; here: the probe of [kept, dropped] with its own cap keeps both, so the check uses
       the weights' order through the cut: cut <= weight of the weakest kept one, by construction of the fade's guard)."""
    rng = np.random.default_rng(seed)
    mx, mv, mp, mo = SI.Module(x), SI.Module(var), SI.Module(probe), SI.Module(oldv)
    a_ok = a_n = b_ok = b_n = c_set = c_n = c_old = faded = 0; c_rel = 0.0
    it = tries = 0
    while it < n and tries < 20 * n:
        tries += 1
        np_, ns_ = int(rng.integers(0, 13)), int(rng.integers(0, 5)); model = int(rng.choice([0, 0, 2]))
        inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, it % 4 == 3, (np_, ns_, model))
        ids = [int(i) for i in rng.permutation(LM.NL)[:np_ + ns_]]
        ub = with_list(ub, ids, np_)
        if it % 4: ub = dense(rng, inp, ub, tx, ids, np_)
        if not lit(mx, inp, ub, tx): continue   # unlit / sky pixel: no light is read (the plain proofs cover it)
        it += 1
        kept, fade, cut, fpos = probe_of(mp, inp, ub, tx)
        assert len(kept) <= cap
        v = mv.run(inp, ub, tx)
        if cut == 0:
            a_n += 1; a_ok += LM.same(v, mx.run(inp, ub, tx))
        else:
            b_n += 1; faded += bool(fade < 1)
            c13 = bytearray(ub[76]); kid = []
            for q in kept:
                li = ids[q]
                if q == fpos and fade != 1:   # a copy of the faded light with its colour x fade (float32, as the shader's B.xyz * lf)
                    j = LM.NL
                    c13[48 * j:48 * j + 48] = c13[48 * li:48 * li + 48]
                    col = np.frombuffer(bytes(c13[48 * li + 16:48 * li + 28]), np.float32) * np.float32(fade)
                    c13[48 * j + 16:48 * j + 28] = col.astype(np.float32).tobytes(); li = j
                if fade > 0 or q != fpos: kid.append(li)
            u2 = with_list(ub, kid, sum(1 for q in kept if q < np_ and (fade > 0 or q != fpos))); u2[76] = bytes(c13)
            b_ok += LM.same(v, mx.run(inp, u2, tx))
        # C: a shuffle inside the points and inside the spots
        perm = [int(i) for i in rng.permutation(np_)] + [np_ + int(i) for i in rng.permutation(ns_)]
        sh = [ids[i] for i in perm]; u3 = with_list(ub, sh, np_)
        k3, f3, cut3, _ = probe_of(mp, inp, u3, tx); c_n += 1
        c_set += sorted(ids[q] for q in kept) == sorted(sh[q] for q in k3) and f3.tobytes() == fade.tobytes() and cut3.tobytes() == cut.tobytes()
        v3 = mv.run(inp, u3, tx)
        a0, a1 = np.array(v[0][:3], np.float64), np.array(v3[0][:3], np.float64)
        c_rel = max(c_rel, float(np.max(np.abs(a0 - a1) / np.maximum(np.abs(a0), 1e-6))))
        b0, b1 = np.array(mo.run(inp, ub, tx)[0][:3], np.float64), np.array(mo.run(inp, u3, tx)[0][:3], np.float64)
        c_old += bool(np.max(np.abs(b0 - b1) / np.maximum(np.abs(b0), 1e-3)) > 0.01)
    print('LCAP=%d strongest: A nothing dropped == x bit for bit %d / %d; B dropped == x on the kept lights with the fade %d / %d (a light '
          'fading in %d); C shuffled list: same kept set and fade bits %d / %d, output max rel. diff %.1e (the old first-N cap: output '
          'changed by > 1 %% in %d / %d)' % (cap, a_ok, a_n, b_ok, b_n, faded, c_set, c_n, c_rel, c_old, c_n))
    return (a_n - a_ok) + (b_n - b_ok) + (c_n - c_set) + int(c_rel > 1e-5) + int(b_n == 0) + int(it < n)


def sweep(x, var, oldv, cap, seed, steps=64, runs=8):
    """continuity: the first light of the list walks from outside its range (1.3 r) to 0.3 r of the pixel, the others (around the pixel)
    stay; the largest step-to-step change of the pixel's luminance, relative to the luminance, for x / the old cap / the new cap. x changes
    smoothly (it has every light); the old cap jumps where the walker starts lighting the pixel and pushes the N-th light out (a pop);
    the new cap must follow x's smoothness (its fade)"""
    rng = np.random.default_rng(seed); mx, mv, mo = SI.Module(x), SI.Module(var), SI.Module(oldv)
    worst = {'x': 0.0, 'old': 0.0, 'new': 0.0}; done = tries = 0
    while done < runs and tries < 50 * runs:
        tries += 1
        np_ = cap + 3
        inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, False, (np_, 0, 0))
        ids = [int(i) for i in rng.permutation(LM.NL)[:np_]]; ub = dense(rng, inp, with_list(ub, ids, np_), tx, ids, np_)
        if not lit(mx, inp, ub, tx) or luma(mx.run(inp, ub, tx)) <= 1e-4: continue
        w = ids[0]   # the walker is the first of the list (the old cap keeps it whenever it lights the pixel)
        c13 = bytearray(ub[76]); A = np.frombuffer(bytes(c13[48 * w:48 * w + 16]), np.float32).copy()
        r = 1.0 / float(A[3]); u = LM.unit(rng); wp = wp_of(inp, ub, tx)
        prev = {}; moved = 0
        for s_ in range(steps):
            t = 1.3 - 1.0 * s_ / (steps - 1)   # from outside its range (1.3 r) to 0.3 r of the pixel
            c13[48 * w:48 * w + 12] = (wp + u * r * t).astype(np.float32).tobytes(); uu = dict(ub); uu[76] = bytes(c13)
            y = {'x': luma(mx.run(inp, uu, tx)), 'old': luma(mo.run(inp, uu, tx)), 'new': luma(mv.run(inp, uu, tx))}
            for k in y:
                if k in prev: worst[k] = max(worst[k], abs(y[k] - prev[k]) / max(abs(prev[k]), abs(y[k]), 1e-6))
            moved += 'x' in prev and y['x'] != prev['x']
            prev = y
        done += moved > 0
    print('LCAP=%d sweep (a light walks from outside its range to 0.3 of it, %d steps x %d): largest step-to-step luminance change: x %.3f, old first-N cap %.3f, '
          'new cap %.3f' % (cap, steps, done, worst['x'], worst['old'], worst['new']))
    return worst


def with_ref(rng, ub, tx):
    """sun colour and ambient on (the fixed scenarios zero them): the light a pixel has without the clustered lights"""
    ub = dict(ub); c16 = bytearray(ub[82])
    for r_ in range(3): struct.pack_into('<f', c16, 16 * r_ + 8, float(rng.uniform(0, 3)))
    ub[82] = bytes(c16); tx = dict(tx); tx[52] = LM.BigTex(LM.table(rng, 0.0, 2.0), LM.FW, LM.FH)
    return ub, tx


def cost_strong(x, mods, n=30):
    """executed instructions per lit pixel: x against the caps, by lights in the cluster (random lights: some light the pixel)"""
    rng = np.random.default_rng(5); ms = {k: SI.Module(v) for k, v in mods.items()}; mx = SI.Module(x)
    print('  cost (instructions / lit pixel, mean of %d lit pixels): lights | x | ' % n + ' | '.join(mods))
    rows = []
    for np_, ns_, dn in ((0, 0, 0), (2, 0, 0), (4, 0, 0), (8, 0, 0), (12, 0, 0), (8, 4, 0), (12, 4, 0),
                         (2, 0, 1), (4, 0, 1), (6, 0, 1), (8, 0, 1), (12, 0, 1), (8, 4, 1), (12, 4, 1)):
        ex = {k: [] for k in ['x'] + list(mods)}; got = 0
        while got < n:
            inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, False, (np_, ns_, int(rng.choice([0, 0, 2]))))
            ids = [int(i) for i in rng.permutation(LM.NL)[:np_ + ns_]]; ub = with_list(ub, ids, np_)
            if dn: ub = dense(rng, inp, ub, tx, ids, np_)
            ub, tx = with_ref(rng, ub, tx)
            if not lit(mx, inp, ub, tx): continue
            got += 1; ex['x'].append(mx.executed)
            for k, m in ms.items(): m.run(inp, ub, tx); ex[k].append(m.executed)
        rows.append((np_, ns_, dn, {k: float(np.mean(v)) for k, v in ex.items()}))
        print('    %2d point + %d spot %s | %4.0f | ' % (np_, ns_, 'dense ' if dn else 'sparse', np.mean(ex['x'])) + ' | '.join('%4.0f' % np.mean(ex[k]) for k in mods))
    return rows


def build_strong(a, tr, x):
    """the strongest-N cap, built and proven, NOT shipped (--only strong): it costs more than no cap (see the head comment)"""
    bad = 0; mods = {}
    for cap in (2, 4):
        d = ['-DLCAP=%d' % cap]
        v = compile_src(strong_source(cap), d, tr)
        pz = compile_src(strong_source(cap, poison=True), d, tr)
        pr = compile_src(strong_source(cap, probe=True), d)
        old = compile_src(capped_source(), d, tr)
        # (not written to dm/android/shaders: measured and rejected - cost_strong; kept here as the record of the measurement)
        print('strongest cap %d: %d / %d words with poison (x %d), interface = the translated one' % (cap, len(v) // 4, len(pz) // 4, len(x) // 4))
        bad += prove_strong(x, v, pr, old, cap, a.n, 0x5e00 + cap)
        sweep(x, v, old, cap, 0x5e10 + cap)
        mods['old %d' % cap] = old; mods['strong %d' % cap] = v
    cost_strong(x, mods)
    return bad


# ---------------------------------------------------------------------------------------------------------------------------------------
# [DOS2] gpu2 DM_VK_NATIVE_LCAP_SEL=1 (default): "drop the weak lights of the pixel" in ONE pass, every light judged on its own.
# Measured above (cost_strong): the strongest-N cap costs MORE than no cap at all - its pass 1 (range, facing, falloff, cone, the weight,
# the insertion) is ~80 % of a light's work in the interpreter, and the kept lights are computed again; it gains nothing before a pixel
# has about 3 x N lights on it. So the default cap judges each light alone against the light the pixel already has:
#   bound  = the largest value this light can add to the pixel: max|colour| x attenuation (nl, falloff, cone: x's own numbers) x the BRDF's
#            peak (max(dc) kd + max(F) D_peak / 4; D_peak = pw / 2pi for Blinn-Phong; the anisotropic GGX has no such bound: no gate there) -
#            no N.H / anisotropy term, so never below what the light really adds;
#   ref    = the pixel's light without the clustered lights (ambient + sun + emissive, after exposure; max component), known before the loop;
#   drop   when bound x exposure < tau x ref (the light adds at most tau of what is there), fade linearly up to 2 tau (no pop), else x's
#          code bit for bit. tau = 1/64 for DM_HACK_LIGHTS=4 ("Средний"), 1/16 for DM_HACK_LIGHTS=2 ("Максимум").
# The decision depends on this light and the pixel only - not on the list order, not on the other lights: stable across frames and across
# cluster borders by construction; continuous in every light's position / colour (the fade). The skipped work is the BRDF lobe (H, the
# lobe's transcendentals, the two sums); the test costs a few operations. Not a count cap: a pixel with many STRONG lights keeps them all
# (the hack buys nothing there); the error is bounded per light (<= 2 tau of the pixel's own light) instead of "the 5th light is gone".
THR_TAU = {2: 1.0 / 16.0, 4: 1.0 / 64.0}


def thr_source(cap, poison=False, probe_tau=None):
    """the 'x' rewrite with the per-light threshold (text patch at build time, as capped_source; the source stays untouched)"""
    s = SRC.read_text(encoding='utf-8')
    tau = THR_TAU[cap] if probe_tau is None else probe_tau
    NLP, NLS = '                [[dont_flatten]] if (nl > 0.0) {\n', '                [[dont_flatten]] if (nl > 0.0 && att != 0.0) {\n'
    SPEC0, SPECA = '        spec = ((ndl * ((F * D) * sc)) * 0.25) * sh;\n', '        spec = ((ndl * ((F * Dv) * sc)) * 0.25) * sh;\n'
    assert s.count(NLP) == 2 and s.count(NLS) == 2 and s.count(SPEC0) == 1 and s.count(SPECA) == 1 and s.count(CAP_AT) == 1
    s = s.replace(CAP_AT, CAP_AT + '#ifdef LTHR\n    float lK = 0.0, lT = 0.0, lpz = 0.0;   // [DOS2] gpu2 DM_VK_NATIVE_LCAP_SEL=1: BRDF peak, threshold, poison mark\n#endif\n', 1)
    ref = ('        {   // [DOS2] gpu2 LTHR: tau x (the pixel\'s light without the clustered lights) / exposure, and this pixel\'s BRDF peak\n'
           '            vec3 lr = amb + fma(max(vec3(0.0), (spec + diff) + E), vec3(ce), U);\n'
           '            lT = (float(LTHR) * max(max(lr.x, lr.y), max(lr.z, 0.0))) / ce;\n'
           '            float mF = max(max(abs(F.x), abs(F.y)), abs(F.z)), mD = max(max(abs(dc.x), abs(dc.y)), abs(dc.z));\n'
           '            lK = fma(mF * %s, 0.25, mD * abs(kd));\n'
           '        }\n')
    s = s.replace(SPEC0, SPEC0 + '#ifdef LTHR\n' + ref % '(pw * 0.159154937)' + '#endif\n', 1)
    # (the anisotropic GGX lobe has no cheap peak: its tangent need not be orthogonal to N, so 1 / (pi at ab dn^2) is unbounded -
    #  the gate stays open there, lT 0: those pixels draw every light with x's code, exact)
    # every light: the bound from x's own numbers (points: nl, falloff, the 1 / (4 pi) factor; spots: att / pi), the fade, the skip
    def gate(body_start, kind):
        if kind == 'p':
            b = ('                    float lqq = dd * rq; float lfa = clamp(fma(lqq, -lqq, one), 0.0, 1.0);\n'
                 '                    vec3 lB = c13._m0[li + 1u].xyz;\n'
                 '                    float lv = (max(max(abs(lB.x), abs(lB.y)), abs(lB.z)) * ((nl * ((lfa * lfa) * rcpz(max(dd, eps)))) * 0.0795774683)) * lK;\n')
        else:
            b = '                    float lv = (max(max(abs(B.x), abs(B.y)), abs(B.z)) * (att * 0.318309873)) * lK;\n'
        b += ('                    float lf = lT > 0.0 ? clamp((lv - lT) / lT, 0.0, 1.0) : 1.0;   // tau .. 2 tau: fades in; 1 = x\'s code\n'
              '                    lpz = max(lpz, 1.0 - lf);\n'
              '                    [[dont_flatten]] if (lf > 0.0) {\n')
        return '#ifdef LTHR\n' + b + '#endif\n'
    out, i = [], 0
    for _ in range(4):
        jp, js = s.find(NLP, i), s.find(NLS, i)
        j, hdr, kind = (jp, NLP, 'p') if jp >= 0 and (js < 0 or jp < js) else (js, NLS, 's')
        e = s.index('\n                }\n', j)   # the block's closing brace (16 spaces; inner blocks are deeper)
        body = s[j + len(hdr):e + 1]
        out.append(s[i:j + len(hdr)] + gate(j, kind) + body + '#ifdef LTHR\n                    }\n#endif\n')
        i = e + 1
    s = ''.join(out) + s[i:]
    # the colour x the fade (x 1.0 exact): in the gated bodies only
    s = s.replace('                    vec3 col = c13._m0[li + 1u].xyz * a;\n', '#ifdef LTHR\n                    vec3 col = (c13._m0[li + 1u].xyz * lf) * a;\n#else\n                    vec3 col = c13._m0[li + 1u].xyz * a;\n#endif\n')
    s = s.replace('                    vec3 col = B.xyz * (att * 0.318309873);\n', '#ifdef LTHR\n                    vec3 col = (B.xyz * lf) * (att * 0.318309873);\n#else\n                    vec3 col = B.xyz * (att * 0.318309873);\n#endif\n')
    if poison:
        FIN = '    outColor = vec4(LF_DF(amb + fma(max(vec3(0.0), (spec + diff) + E), vec3(ce), U)), 1.0);\n'
        assert s.count(FIN) == 1
        s = s.replace(FIN, FIN + '    [[dont_flatten]] if (lpz > 0.0) { float lm = max(max(outColor.r, outColor.g), max(outColor.b, 0.05)) * (1.0 + lpz); outColor = vec4(lm, 0.0, lm, 1.0); }\n')
    return s, tau


def thr_defines(cap, tau=None):
    t = THR_TAU[cap] if tau is None else tau
    return ['-DLCAP=%d' % cap, '-DLTHR=%r' % float(t)]


def prove_thr(x, var, var0, oldv, cap, tau, n, seed):
    """A. tau 0 (the same text, LTHR=0.0): == x bit for bit (the gate and the fade change nothing when nothing is dropped);
    B. the error is bounded as designed: x - variant <= (number of lights) x 2 tau x ref per component (ref = x with an empty list:
       ambient + sun + emissive after exposure; the max component), reported as the largest ratio (must stay <= 1);
    C. the list shuffled (points among points, spots among spots): the same output up to the sum order (<= 1e-5 relative); the old
       first-N cap counted as before (output changed by > 1 %);
    D. how much is dropped: cases where the variant differs from x."""
    rng = np.random.default_rng(seed)
    mx, mv, m0, mo = SI.Module(x), SI.Module(var), SI.Module(var0), SI.Module(oldv)
    a_ok = c_n = c_old = differ = 0; b_worst = 0.0; c_rel = 0.0; it = tries = 0
    while it < n and tries < 20 * n:
        tries += 1
        np_, ns_ = int(rng.integers(0, 13)), int(rng.integers(0, 5)); model = int(rng.choice([0, 0, 2]))
        inp, ub, tx = LM.case('ClusteredLightingWithSSAO', rng, False, (np_, ns_, model))
        if it % 2: ub, tx = with_ref(rng, ub, tx)   # sun and ambient on: the reference the threshold is measured against
        ids = [int(i) for i in rng.permutation(LM.NL)[:np_ + ns_]]
        ub = with_list(ub, ids, np_)
        if it % 4: ub = dense(rng, inp, ub, tx, ids, np_)
        if not lit(mx, inp, ub, tx): continue
        it += 1
        ox, ov = mx.run(inp, ub, tx), mv.run(inp, ub, tx)
        a_ok += LM.same(m0.run(inp, ub, tx), ox)
        differ += not LM.same(ov, ox)
        ref = mx.run(inp, with_list(ub, [], 0), tx)[0]
        rm = max(max(ref[:3]), 0.0)
        d = max(float(ox[0][k]) - float(ov[0][k]) for k in range(3))
        if rm > 0 and np_ + ns_: b_worst = max(b_worst, d / ((np_ + ns_) * 2 * tau * rm))
        elif d > 0: b_worst = max(b_worst, float('inf'))
        perm = [int(i) for i in rng.permutation(np_)] + [np_ + int(i) for i in rng.permutation(ns_)]
        u3 = with_list(ub, [ids[i] for i in perm], np_); c_n += 1
        v3 = mv.run(inp, u3, tx)
        a0, a1 = np.array(ov[0][:3], np.float64), np.array(v3[0][:3], np.float64)
        c_rel = max(c_rel, float(np.max(np.abs(a0 - a1) / np.maximum(np.abs(a0), 1e-6))))
        b0, b1 = np.array(mo.run(inp, ub, tx)[0][:3], np.float64), np.array(mo.run(inp, u3, tx)[0][:3], np.float64)
        c_old += bool(np.max(np.abs(b0 - b1) / np.maximum(np.abs(b0), 1e-3)) > 0.01)
    print('LCAP=%d threshold (tau 1/%d): A tau 0 == x bit for bit %d / %d; B largest error / designed bound %.3f (<= 1); C shuffled list: '
          'output max rel. diff %.1e (the old first-N cap: output changed by > 1 %% in %d / %d); D the variant dropped / faded something in %d / %d'
          % (cap, round(1 / tau), a_ok, it, b_worst, c_rel, c_old, c_n, differ, it))
    return (it - a_ok) + int(b_worst > 1.0) + int(c_rel > 1e-5) + int(it < n)


def build_thr(a, tr, x):
    """DM_VK_NATIVE_LCAP_SEL=1 (default) / 3 modules: light_lcap<N>t.frag.spv.inc (threshold), light_lcap<N>tp (the same + poison)"""
    bad = 0; mods = {}
    for cap in (2, 4):
        src, tau = thr_source(cap)
        v = compile_src(src, thr_defines(cap), tr)
        v0 = compile_src(src, thr_defines(cap, 0.0), tr)
        pz = compile_src(thr_source(cap, poison=True)[0], thr_defines(cap), tr)
        old = compile_src(capped_source(), ['-DLCAP=%d' % cap], tr)
        write_inc(v, OUT / ('light_lcap%dt.frag.spv.inc' % cap), '-DLCAP=%d -DLTHR=%r threshold (thr_source)' % (cap, tau))
        write_inc(pz, OUT / ('light_lcap%dtp.frag.spv.inc' % cap), '-DLCAP=%d -DLTHR=%r threshold + poison (thr_source poison)' % (cap, tau))
        print('built light_lcap%dt / %dtp.frag.spv.inc: %d / %d words (x %d), interface = the translated one' % (cap, cap, len(v) // 4, len(pz) // 4, len(x) // 4))
        bad += prove_thr(x, v, v0, old, cap, tau, a.n, 0x7e00 + cap)
        sweep(x, v, old, cap, 0x7e10 + cap)
        mods['old %d' % cap] = old; mods['thr %d' % cap] = v
    cost_strong(x, mods)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bundles', default=str(WORK / 'shaders-vk'))
    ap.add_argument('--n', type=int, default=200)
    ap.add_argument('--only', choices=['old', 'strong', 'thr'], default=None)
    a = ap.parse_args()
    tr = read_bundle(Path(a.bundles) / (KEY + '.xgs'))
    x = compile_([], tr); bad = 0
    if a.only in (None, 'old'):
        big = compile_(['-DLCAP=99'], tr)
        for cap in (2, 4):
            v = compile_(['-DLCAP=%d' % cap], tr)
            write_inc(v, OUT / ('light_lcap%d.frag.spv.inc' % cap), '-DLCAP=%d' % cap)
            print('built light_lcap%d.frag.spv.inc: %d words (x %d words), interface = the translated one' % (cap, len(v) // 4, len(x) // 4))
            bad += prove(x, v, big, cap, a.n, 0x11c0 + cap)
        cost(x)
    if a.only == 'strong': bad += build_strong(a, tr, x)   # (measured and rejected: not shipped)
    if a.only in (None, 'thr'): bad += build_thr(a, tr, x)
    if bad: raise SystemExit('%d failures' % bad)


if __name__ == '__main__':
    main()
