"""NR-POSONLY (DM_NR_POSONLY, docs/dos2/nr/NR-POSONLY.md): which depth-only pipelines could fetch a compact position-only vertex stream,
proved on the player's translated pack, and what it would save.

For every depth-only pipeline of a warm-up list (vk-pipeline-list.bin: the device's own CreateInfo words, tools/dos2_pipe_seed.py decode)
the vertex shader of the pack is checked two ways:
  static  the same conservative dependence walk the backend runs (xbg_spirv_posonly.cpp): every value is tagged with the Location inputs it
          may depend on (through SSA ids, Function / Private variables flow-insensitively, every branch / switch condition of the module);
          the shader is position-only when gl_Position and every other built-in output depend on Location 0 only, nothing writes memory
          but Output / Function / Private variables and there is one function. Location outputs do not count: the depth-only draws this is
          for have an empty fragment shader (NR-33B twins) or one that reads nothing.
  interp  tools/spv_interp.py runs the shader with the same Location 0 and uniforms and two different random values for every other input,
          --n times: gl_Position must be bit for bit the same. A static "position-only" the interpreter contradicts = FAIL (exit 1).
Per pipeline: binding stride of Location 0, the size of its format, the ratio. Weighted by the heat log when given (--heat shader-heat.tsv:
draws per program, NR-HEAT format) else per pipeline.

  dos2_posonly_check.py --list vk-pipeline-list.bin --pack shaders.xgspack [--n 6] [--heat shader-heat.tsv] [--out posonly.tsv]
"""
import argparse, collections, random, struct, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import numpy as np
import spv_interp as SI
import dos2_pipe_seed as PS
from dos2_nr_mat import pack_index, bundle_parts

FMT_SIZE = {90: 6, 106: 12, 103: 8, 109: 16, 83: 4, 37: 4, 41: 4, 97: 8, 98: 8, 100: 8, 107: 16, 108: 16, 64: 4, 77: 4, 38: 4, 44: 4, 70: 4, 91: 8, 92: 8, 95: 8, 96: 8, 104: 12, 105: 12, 99: 12}
OP_FUNCTION, OP_FUNCTION_END, OP_VARIABLE, OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_IN_BOUNDS_AC, OP_PTR_AC = 54, 56, 59, 61, 62, 65, 66, 67
OP_BRANCH_COND, OP_SWITCH, OP_FUNCTION_CALL, OP_IMAGE_WRITE, OP_COPY_MEMORY, OP_KILL = 250, 251, 57, 99, 63, 252
OP_ATOMIC = set(range(227, 243)) | {4407}
SC_INPUT, SC_OUTPUT, SC_PRIVATE, SC_FUNCTION = 1, 3, 6, 7
NO_RESULT = {OP_STORE, 62, 247, 248, 249, 250, 251, 252, 253, 254, 255, 71, 72, 14, 15, 16, 17, 5, 6, 7, 11, 3, 2, 1, 0, 8, 10, 4}


def static_posonly(words):
    """(position-only, why, dependence mask of gl_Position) - the walk of xbg_spirv_posonly.cpp in Python (kept in step with it)"""
    w = words; i = 5; loc = {}; builtin = {}; sc = {}; ptr_root = {}; dep = collections.defaultdict(int); nfunc = 0
    insts = []
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if n == 0: return False, 'malformed', 0
        a = w[i + 1:i + n]
        if op == 71 and len(a) >= 3:
            if a[1] == 30: loc[a[0]] = a[2]
            if a[1] == 11: builtin[a[0]] = a[2]
        if op == 72 and len(a) >= 4 and a[2] == 11: builtin[('m', a[0])] = a[3]
        if op == OP_FUNCTION: nfunc += 1
        if op == OP_VARIABLE: sc[a[1]] = a[2]; ptr_root[a[1]] = a[1]
        insts.append((op, a)); i += n
    if nfunc != 1: return False, 'functions %d' % nfunc, 0
    for v, s in sc.items():
        if s == SC_INPUT and v in loc: dep[v] = 1 << loc[v]
    ctrl = 0
    changed = True; rounds = 0
    while changed:
        changed = False; rounds += 1
        if rounds > 64: return False, 'no fixpoint', 0
        for op, a in insts:
            if op in (OP_IMAGE_WRITE,) or op in OP_ATOMIC: return False, 'writes memory (op %d)' % op, 0
            if op == OP_KILL: continue
            if op == OP_FUNCTION_CALL: return False, 'call', 0
            if op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_AC, OP_PTR_AC):
                rid, base = a[1], a[2]; root = ptr_root.get(base, base)
                if ptr_root.get(rid) != root: ptr_root[rid] = root; changed = True
                d = dep[root]
                for x in a[3:]: d |= dep[x]
                if d != dep[rid]: dep[rid] = d; changed = True
                continue
            if op == OP_LOAD:
                rid, p = a[1], a[2]; root = ptr_root.get(p, p); d = dep[root] | dep[p]
                if d != dep[rid]: dep[rid] = d; changed = True
                continue
            if op == OP_STORE:
                p, val = a[0], a[1]; root = ptr_root.get(p, p)
                s = sc.get(root)
                if s not in (SC_OUTPUT, SC_PRIVATE, SC_FUNCTION): return False, 'store to storage class %s' % s, 0
                d = dep[root] | dep[val] | dep[p]
                if d != dep[root]: dep[root] = d; changed = True
                continue
            if op == OP_COPY_MEMORY: return False, 'copy memory', 0
            if op == OP_BRANCH_COND or op == OP_SWITCH:
                d = ctrl | dep[a[0]]
                if d != ctrl: ctrl = d; changed = True
                continue
            if op in NO_RESULT or len(a) < 2: continue
            if op in (21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 19, 20, 43, 44, 45, 46, 41, 42, 48, 49, 50, 51, 52): continue   # types / constants
            if op == OP_VARIABLE: continue
            rid = a[1]; d = 0
            ops = a[2:]
            if op == 79: ops = a[2:4]          # OpVectorShuffle: two vectors, then literal components
            elif op == 81: ops = a[2:3]        # OpCompositeExtract: composite, then literal indexes
            elif op == 82: ops = a[2:4]        # OpCompositeInsert: object, composite, then literal indexes
            elif op == 12: ops = a[4:]         # OpExtInst: set, literal instruction, operands
            for x in ops: d |= dep[x]
            if d != dep[rid]: dep[rid] = d; changed = True
    # outputs that stay for a depth-only draw: built-in outputs (gl_Position, ClipDistance, CullDistance, PointSize): their roots
    outd = 0; have_pos = False
    for v, s in sc.items():
        if s != SC_OUTPUT: continue
        if v in loc: continue   # Location outputs: no fragment shader reads them
        outd |= dep[v] | ctrl; have_pos = True
    if not have_pos: return False, 'no built-in output', 0
    if outd & ~1: return False, 'gl_Position depends on locations %#x' % outd, outd
    return True, '', outd


def interp_posonly(spirv, n, seed):
    m = SI.Module(spirv)
    ins = [m.dec[v][30][0] for v, (pt, s) in m.vars.items() if s == 1 and 30 in m.dec.get(v, {})]
    ubo = sorted({m.dec.get(v, {}).get(33, [None])[0] for v, (pt, s) in m.vars.items() if s == 2})
    if 0 not in ins: return None, 'no location 0'
    same = 0
    for k in range(n):
        rng = random.Random(seed + k)
        base = {l: tuple(rng.uniform(-2, 2) for _ in range(4)) for l in ins}
        ubos = {b: np.array([rng.uniform(-2, 2) for _ in range(4096)], dtype=np.float32).tobytes() + b'\0' * (65536 - 16384) for b in ubo}
        bi = {('bi', 42): rng.randrange(0, 1 << 16), ('bi', 43): rng.randrange(0, 64)}
        outs = []
        for t in range(2):
            r2 = random.Random(seed * 7 + k * 31 + t)
            inp = dict(base); inp.update(bi)
            for l in ins:
                if l != 0: inp[l] = tuple(r2.uniform(-50, 50) for _ in range(4))
            o = m.run(inp, ubos=dict(ubos))
            pos = {key: val for key, val in o.items() if not isinstance(key, int)}
            outs.append(repr({key: [SI.f2u(x) if isinstance(x, (float, np.floating)) else x for x in (val if isinstance(val, list) else [val])] for key, val in pos.items()}))
        same += outs[0] == outs[1]
    return same == n, '%d of %d runs equal' % (same, n)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--list', required=True); ap.add_argument('--pack', required=True); ap.add_argument('--n', type=int, default=6)
    ap.add_argument('--heat'); ap.add_argument('--out'); ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--all-vs', action='store_true', help='every vertex shader of the pack: the static walk, and the interpreter on each one it accepts (soundness)')
    a = ap.parse_args()
    data, idx = pack_index(a.pack)
    if a.all_vs:
        n = acc = refuted = interp_err = 0; why = collections.Counter()
        for k, (o, size, _) in idx.items():
            st, binds, spv = bundle_parts(data[o:o + size])
            if st != 0 or not spv: continue
            n += 1; ok_s, why_s, _ = static_posonly(struct.unpack('<%dI' % (len(spv) // 4), spv))
            if not ok_s: why[why_s.split(' on ')[0][:40]] += 1; continue
            acc += 1
            try: ok_i, _ = interp_posonly(spv, a.n, int(k[:8], 16))
            except Exception: interp_err += 1; continue
            if ok_i is False: refuted += 1; print('FAIL %s: static position-only, the interpreter moved gl_Position' % k)
        print('all vertex shaders %d: static position-only %d (interpreter refuted %d, not interpretable %d); refusals %s' % (n, acc, refuted, interp_err, why.most_common(6)))
        return 1 if refuted else 0
    recs, _ = PS.records(Path(a.list).read_bytes(), 0)
    rows = []; seen_vs = {}; fail = []
    tot = collections.Counter(); ratio_w = collections.Counter()
    for keys, w, raw in recs:
        try: p, _ = PS.decode(w)
        except Exception: continue
        wm = [b[7] for b in p['blend']]
        if not (p['depth'] is not None and (len(p['col']) == 0 or all(m == 0 for m in wm))): continue
        va = {x[0]: x for x in p['va']}; vb = {x[0]: x for x in p['vb']}
        if 0 not in va: continue
        loc0 = va[0]; stride = vb[loc0[1]][1] if loc0[1] in vb else 0; psz = FMT_SIZE.get(loc0[2], 0)
        inst = any(b[2] == 1 for b in p['vb'])
        vk = keys[0].split('#')[0]
        tot['depth-only pipelines'] += 1
        if vk not in idx: tot['vs not in pack'] += 1; continue
        if vk not in seen_vs:
            st, binds, spv = bundle_parts(data[idx[vk][0]:idx[vk][0] + idx[vk][1]])
            ws = struct.unpack('<%dI' % (len(spv) // 4), spv)
            ok_s, why_s, d = static_posonly(ws)
            try: ok_i, why_i = interp_posonly(spv, a.n, int(vk[:8], 16))
            except (NotImplementedError, KeyError, ValueError, TypeError, RuntimeError, IndexError) as e: ok_i, why_i = None, 'interp: %s' % str(e)[:60]
            if ok_s and ok_i is False: fail.append('%s: static says position-only, the interpreter differs (%s)' % (vk, why_i))
            seen_vs[vk] = (ok_s, why_s, ok_i, why_i)
        ok_s, why_s, ok_i, why_i = seen_vs[vk]
        elig = ok_s and not inst and psz and stride > psz
        tot['static position-only'] += ok_s; tot['interp equal'] += bool(ok_i); tot['instanced binding'] += inst
        tot['eligible (static, not instanced, stride > position)'] += bool(elig)
        if elig: ratio_w[(stride, psz)] += 1
        rows.append('%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s\t%s' % (vk, keys[1] if len(keys) > 1 else '-', stride, psz, len(p['va']), int(ok_s), why_s, ok_i, why_i))
        if a.limit and len(rows) >= a.limit: break
    vs_static = sum(1 for v in seen_vs.values() if v[0]); vs_interp = sum(1 for v in seen_vs.values() if v[2]); vs_n = len(seen_vs)
    print('depth-only pipelines: %s' % dict(tot))
    print('distinct vertex shaders %d: static position-only %d, interpreter equal %d (static misses %d), FAIL %d' % (vs_n, vs_static, vs_interp,
          sum(1 for v in seen_vs.values() if v[2] and not v[0]), len(fail)))
    if ratio_w:
        s = sum(ratio_w.values()); avg = sum((st / ps) * n for (st, ps), n in ratio_w.items()) / s
        print('eligible pipelines by (stride, position bytes): %s; mean fetch ratio %.2fx' % (ratio_w.most_common(8), avg))
    for f in fail[:20]: print('FAIL', f)
    if a.out: Path(a.out).write_text('vs\tfs\tstride\tpos_bytes\tattrs\tstatic\twhy\tinterp\tinterp_why\n' + '\n'.join(rows) + '\n', encoding='utf-8')
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
