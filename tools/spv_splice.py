"""SPIR-V splice (NB-18b-3): replace a consumer shader's texture read with a call to a producer fragment shader.

Works on `spirv-dis --raw-id` text of spirv-opt -O modules (the translated DOS2 post shaders). splice(host, producer, ...) returns the
text of one module: the host's code unchanged, except that every image instruction reading the host texture binding `site` becomes

    OpStore <producer input as Private> <value>   (per producer input: the fragment's gl_FragCoord, the read's own coordinate,
                                                   a host input location, or a constant)
    OpFunctionCall <producer main>                 (the producer's main, its Input / Output variables turned into Private ones)
    %result = OpLoad <producer output location 0>  (optionally re-quantised to UNORM8: round-trip of an RGBA8 render target)

The producer's descriptors are renumbered (bind_map: old binding -> new binding); its types / constants are shared with the host's when
equal (non-aggregate types must be unique). spirv-opt -O afterwards inlines the call, so the fused code is the two translated codes
back to back: bit-identical arithmetic by construction (the equivalence test in tools/dos2_post_fuse.py checks it with spv_interp.py).
A producer of None with const=(r, g, b, a) replaces the read with that constant (the "3D only" combine: UI texel = 0).

NB-18b-8 (route A, F1A) additions:
  sites=[(binding, only, inputs, quant)]  several read sites served by ONE import of the producer, each with its own inputs; `only` =
                                          None (every read of that binding) or a set of result ids (just those image instructions).
  input spec ('value', id)                a vec4 result id of the host that dominates the read site.
  input spec 'read'                       the original image instruction is kept (fresh result id) and its vec4 result is the value: the
                                          producer post-processes the read (the luma proxy over the FXAA analysis taps).
  shared bindings                         a producer UniformConstant variable whose (mapped) binding and pointer type equal a host
                                          variable's becomes that variable (no second variable on one binding).
"""
import re

TOK = re.compile(r'"(?:[^"\\]|\\.)*"|\S+')
HEAD_OPS = {'OpCapability', 'OpExtension', 'OpExtInstImport', 'OpMemoryModel', 'OpEntryPoint', 'OpExecutionMode', 'OpSource',
            'OpSourceExtension', 'OpSourceContinued', 'OpName', 'OpMemberName', 'OpString', 'OpModuleProcessed'}
ANNOT_OPS = {'OpDecorate', 'OpMemberDecorate', 'OpDecorationGroup', 'OpGroupDecorate', 'OpGroupMemberDecorate', 'OpDecorateString',
             'OpMemberDecorateString', 'OpDecorateId'}
UNIQUE_TYPES = {'OpTypeVoid', 'OpTypeBool', 'OpTypeInt', 'OpTypeFloat', 'OpTypeVector', 'OpTypeMatrix', 'OpTypeImage', 'OpTypeSampler',
                'OpTypeSampledImage', 'OpTypePointer', 'OpTypeFunction'}
CONSTS = {'OpConstant', 'OpConstantTrue', 'OpConstantFalse', 'OpConstantComposite', 'OpConstantNull'}
IMAGE_READS = {'OpImageFetch', 'OpImageSampleImplicitLod', 'OpImageSampleExplicitLod', 'OpImageGather', 'OpImageRead'}


class Ins:
    __slots__ = ('res', 'op', 'args')

    def __init__(self, res, op, args): self.res, self.op, self.args = res, op, list(args)

    def text(self): return ('%s = ' % self.res if self.res else '') + ' '.join([self.op] + self.args)


def parse(text):
    out = []
    for line in text.splitlines():
        line = line.split(';', 1)[0] if not line.lstrip().startswith('OpName') else line
        t = TOK.findall(line)
        if not t: continue
        if len(t) > 2 and t[1] == '=': out.append(Ins(t[0], t[2], t[3:]))
        else: out.append(Ins(None, t[0], t[1:]))
    return out


class Module:
    def __init__(self, text):
        self.ins = parse(text)
        self.head, self.annot, self.glob, self.funcs = [], [], [], []
        fn = None
        for i in self.ins:
            if i.op == 'OpFunction': fn = [i]; self.funcs.append(fn); continue
            if fn is not None:
                fn.append(i)
                if i.op == 'OpFunctionEnd': fn = None
                continue
            (self.head if i.op in HEAD_OPS else self.annot if i.op in ANNOT_OPS else self.glob).append(i)
        self.defs = {i.res: i for i in self.glob if i.res}
        self.decos = {}
        for i in self.annot:
            if i.op == 'OpDecorate': self.decos.setdefault(i.args[0], []).append(i.args[1:])

    def deco(self, id_, name):
        for d in self.decos.get(id_, []):
            if d[0] == name: return d[1:]
        return None

    def entry(self):
        return next(i for i in self.head if i.op == 'OpEntryPoint')

    def var_by_binding(self, b):
        for v, ds in self.decos.items():
            for d in ds:
                if d[0] == 'Binding' and int(d[1]) == b: return v
        return None

    def var_by(self, sc, deco, val):
        for i in self.glob:
            if i.op == 'OpVariable' and i.args[1] == sc:
                d = self.deco(i.res, deco)
                if d is not None and d[0] == str(val): return i.res
        return None

    def text(self):
        out = [i.text() for i in self.head + self.annot + self.glob]
        for f in self.funcs: out += [i.text() for i in f]
        return '\n'.join(out) + '\n'


class Builder:
    """the host module being extended: a key table of its unique types / constants, fresh ids"""
    def __init__(self, host, tag):
        self.m = host; self.n = 0; self.keys = {}; self.tag = tag
        for i in host.glob:
            if i.op in UNIQUE_TYPES or i.op in CONSTS: self.keys[(i.op,) + tuple(i.args)] = i.res
        self.glsl = next((i.res for i in host.head if i.op == 'OpExtInstImport' and i.args[0] == '"GLSL.std.450"'), None)

    def fresh(self, kind='n'):
        self.n += 1; return '%%%s%s%d' % (self.tag, kind, self.n)

    def tdef(self, id_):
        return next(i for i in self.m.glob if i.res == id_)

    def decl(self, op, *args, tag='t'):
        k = (op,) + tuple(args)
        if k in self.keys: return self.keys[k]
        r = self.fresh(tag); self.m.glob.append(Ins(r, op, args)); self.keys[k] = r
        return r

    def glsl_import(self):
        if not self.glsl:
            self.glsl = self.fresh('glsl'); self.m.head.insert(1, Ins(self.glsl, 'OpExtInstImport', ['"GLSL.std.450"']))
        return self.glsl

    # handy types / constants
    def f32(self): return self.decl('OpTypeFloat', '32')
    def vec(self, n): return self.decl('OpTypeVector', self.f32(), str(n))
    def fconst(self, v): return self.decl('OpConstant', self.f32(), repr(float(v)) if v != int(v) else str(int(v)))


def _map(tok, mp): return mp.get(tok, tok) if tok.startswith('%') else tok


def splice(host_text, prod_text, site, tag, bind_map=None, inputs=None, quant=False, const=None, sites=None):
    """host_text / prod_text: spirv-dis --raw-id text. site: host binding whose reads are replaced. inputs: {producer input key
    ('FragCoord' or location int): value spec}, value spec = 'fragcoord' | 'coord' | ('host', location) | ('const', (x, y, z, w)).
    Returns (text, info) - info: {'sites': n, 'added_builtins': [...]}"""
    H = Module(host_text); B = Builder(H, tag); info = {'sites': 0, 'added_builtins': [], 'shared': []}
    SL = [list(x) for x in (sites or [(site, None, inputs, quant)])]
    for x in SL:
        x.append(H.var_by_binding(x[0])); x.append(0)          # [binding, only, inputs, quant, host variable, hits]
        if x[4] is None: raise ValueError('host has no binding %d' % x[0])
    entry = H.entry()
    if prod_text is not None:
        P = Module(prod_text); mp = {}; pins, pout = {}, None
        pre = '%%%s_' % tag
        for i in P.ins:
            if i.res and i.res not in mp: mp[i.res] = pre + i.res[1:]
        for i in P.head:
            if i.op == 'OpCapability' and not any(h.op == 'OpCapability' and h.args == i.args for h in H.head):
                H.head.insert(0, Ins(None, 'OpCapability', i.args))
            if i.op == 'OpExtInstImport': mp[i.res] = B.glsl_import()
        io_vars = {}
        for i in P.glob:
            args = [_map(a, mp) for a in i.args]
            if i.op == 'OpTypePointer' and args[0] in ('Input', 'Output'):
                mp[i.res] = B.decl('OpTypePointer', 'Private', args[1]); continue
            if i.op in UNIQUE_TYPES or i.op in CONSTS:
                mp[i.res] = B.decl(i.op, *args); continue
            if i.op == 'OpVariable' and args[1] in ('Input', 'Output'):
                key = 'FragCoord' if P.deco(i.res, 'BuiltIn') == ['FragCoord'] else int((P.deco(i.res, 'Location') or ['-1'])[0])
                r = mp[i.res]; H.glob.append(Ins(r, 'OpVariable', [args[0], 'Private']))
                if args[1] == 'Output': pout = (r, P.defs[i.args[0]].args[1], key)
                else: pins[key] = (r, P.defs[i.args[0]].args[1])
                io_vars[i.res] = True; continue
            if i.op == 'OpVariable' and args[1] == 'UniformConstant' and P.deco(i.res, 'Binding') is not None:
                nbd = int(P.deco(i.res, 'Binding')[0]); nbd = (bind_map or {}).get(nbd, nbd); hv = H.var_by_binding(nbd)
                if hv is not None and B.tdef(hv).args[0] == args[0]:
                    mp[i.res] = hv; io_vars[i.res] = True; info['shared'].append(nbd); continue
            H.glob.append(Ins(mp.get(i.res), i.op, args))
        for i in P.annot:
            tgt = i.args[0]
            if tgt in io_vars: continue
            mapped = mp.get(tgt)
            if mapped is None or mapped in B.keys.values() and not mapped.startswith(pre): continue   # deduped onto a host id
            a = [_map(x, mp) for x in i.args]
            if i.op == 'OpDecorate' and a[1] == 'Binding':
                old = int(a[2]); a[2] = str((bind_map or {}).get(old, old))
            H.annot.append(Ins(None, i.op, a))
        for f in P.funcs:
            H.funcs.append([Ins(mp.get(i.res) if i.res else None, i.op, [_map(a, mp) for a in i.args]) for i in f])
        pmain = mp[P.entry().args[1]]
        # producer input types: Private pointee type ids, already mapped
        pin_t = {k: (r, _map(t, mp)) for k, (r, t) in pins.items()}
        pout_t = (pout[0], _map(pout[1], mp)) if pout else None
        for x in SL:
            missing = set(pin_t) - set(x[2] or {})
            if missing: raise ValueError('producer inputs without a value: %s' % sorted(missing, key=str))
    void = B.decl('OpTypeVoid')
    # host: every image read of the sites' variables (load -> variable; OpImage of a load -> the same variable)
    loads = {}
    for f in H.funcs:
        for i in f:
            if i.op == 'OpLoad' and any(i.args[1] == x[4] for x in SL): loads[i.res] = i.args[1]
    for f in H.funcs:
        for i in f:
            if i.op == 'OpImage' and i.args[1] in loads: loads[i.res] = loads[i.args[1]]
    fc_var = None
    nhost = len(H.funcs) - (len(P.funcs) if prod_text is not None else 0)      # the producer's own functions are not searched
    for fi, f in enumerate(H.funcs):
        out = []
        for i in f:
            S = None
            if fi < nhost and i.op in IMAGE_READS and i.args[1] in loads:
                S = next((x for x in SL if x[4] == loads[i.args[1]] and (x[1] is None or i.res in x[1])), None)
            if S is not None:
                inputs, quant = S[2], S[3]; S[5] += 1
                info['sites'] += 1; res, rt, coord = i.res, i.args[0], i.args[2]
                if prod_text is None:
                    c = [B.fconst(x) for x in const]
                    out.append(Ins(res, 'OpCopyObject', [rt, B.decl('OpConstantComposite', rt, *c)])); continue
                for key, spec in (inputs or {}).items():
                    var, t = pin_t[key]
                    td = B.tdef(t); ncomp = int(td.args[1]) if td.op == 'OpTypeVector' else 1
                    comps = []
                    if spec == 'fragcoord':
                        if fc_var is None:
                            fc_var = H.var_by('Input', 'BuiltIn', 'FragCoord')
                            if fc_var is None:
                                fc_var = B.fresh('fc'); H.glob.append(Ins(fc_var, 'OpVariable', [B.decl('OpTypePointer', 'Input', B.vec(4)), 'Input']))
                                H.annot.append(Ins(None, 'OpDecorate', [fc_var, 'BuiltIn', 'FragCoord'])); entry.args.append(fc_var)
                                info['added_builtins'].append('FragCoord')
                        v = B.fresh('v'); out.append(Ins(v, 'OpLoad', [B.vec(4), fc_var])); src, sn = v, 4
                    elif spec == 'coord':
                        src = coord; sn = 2
                    elif spec == 'read':
                        src = B.fresh('rd'); out.append(Ins(src, i.op, i.args)); sn = 4
                    elif isinstance(spec, tuple) and spec[0] == 'value':     # a vec4 id of the host that dominates the read
                        src = spec[1]; sn = 4
                    elif isinstance(spec, tuple) and spec[0] == 'host':
                        hv = H.var_by('Input', 'Location', spec[1]); pt = H.defs[H.defs[hv].args[0]].args[1]
                        v = B.fresh('v'); out.append(Ins(v, 'OpLoad', [pt, hv])); src = v
                        sn = int(H.defs[pt].args[1]) if H.defs[pt].op == 'OpTypeVector' else 1
                    elif isinstance(spec, tuple) and spec[0] == 'const':
                        src = None; comps = [B.fconst(x) for x in spec[1][:ncomp]]
                    else: raise ValueError(spec)
                    if src is not None:
                        for k in range(ncomp):
                            if k < sn:
                                e = B.fresh('e'); out.append(Ins(e, 'OpCompositeExtract', [B.f32(), src, str(k)])); comps.append(e)
                            else: comps.append(B.fconst(1.0 if k == 3 else 0.0))
                    if ncomp == 1: val = comps[0]
                    else:
                        val = B.fresh('c'); out.append(Ins(val, 'OpCompositeConstruct', [t] + comps))
                    out.append(Ins(None, 'OpStore', [var, val]))
                out.append(Ins(B.fresh('call'), 'OpFunctionCall', [void, pmain]))
                ovar, ot = pout_t
                if not quant:
                    out.append(Ins(res, 'OpLoad', [ot, ovar])); continue
                # UNORM8 round trip of the elided pass's render target: k = RoundEven(clamp(x, 0, 1) * 255), x' = k / 255
                g = B.glsl_import(); v4 = B.vec(4)
                z, o, s = (B.decl('OpConstantComposite', v4, *[B.fconst(x)] * 4) for x in (0.0, 1.0, 255.0))
                a = B.fresh('q'); out.append(Ins(a, 'OpLoad', [ot, ovar]))
                b = B.fresh('q'); out.append(Ins(b, 'OpExtInst', [v4, g, 'FClamp', a, z, o]))
                c = B.fresh('q'); out.append(Ins(c, 'OpFMul', [v4, b, s]))
                d = B.fresh('q'); out.append(Ins(d, 'OpExtInst', [v4, g, 'RoundEven', c]))
                out.append(Ins(res, 'OpFDiv', [v4, d, s]))
                continue
            out.append(i)
        H.funcs[fi] = out
    for x in SL:
        if not x[5]: raise ValueError('no image read of binding %d in the host%s' % (x[0], '' if x[1] is None else ' among %s' % sorted(x[1])))
    # types first (the builder appended new declarations at the end of glob; SPIR-V needs declaration before use)
    H.glob = _order(H.glob)
    return H.text(), info


def _order(glob):
    """topological order of global declarations (new ones were appended)"""
    defs = {i.res: i for i in glob if i.res}; done, out = set(), []

    def visit(i):
        if i.res in done: return
        done.add(i.res)
        for a in i.args:
            if a in defs and a != i.res: visit(defs[a])
        out.append(i)
    for i in glob:
        if i.res: visit(i)
        else: out.append(i)
    return out
