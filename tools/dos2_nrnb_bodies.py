#!/usr/bin/env python3
# [DOS2] NR-NATIVE step D half b (DM_NRN_NOBODY, docs/dos2/nr/NR-NATIVE-D.md section 12): the text of the NVN bodies the fold of
# dm/gfx/nvn/nvn_nrn_b.inc stands for, cut out of the engine's own sources for the host test
# dm/gfx/vulkan/engine/tests/nr_nrn_b_test.c. The test compiles THIS text (not a copy kept by hand) next to the fold and runs both on
# the same states, so a change of a body that the fold does not follow fails the test.
#
#   python tools/dos2_nrnb_bodies.py --out <file>     write the generated include
#   python tools/dos2_nrnb_bodies.py --hash           print one FNV-1a 64 per piece (source_structure_test.py pins them: a changed body
#                                                     means "look at nvn_nrn_b.inc again, then update the pin")
import argparse, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / 'dm' / 'gfx' / 'nvn'
# (file, the start of the piece's first line). A '#define' is taken with its continuation lines, a line that ends in ';' and opens no
# brace alone, anything else up to the brace that closes the first one it opens.
PIECES = [
    # the tap: which command buffer it follows, its count, its binding table
    ('nvn_nb_tap.inc', '#define NBT(x) do {'),
    ('nvn_nb_tap.inc', 'static nb_frame *nbt_f(NVNcommandBuffer *c) {'),
    ('nvn_nb_tap.inc', 'static void nbt_note(NVNcommandBuffer *c, int what)'),
    # NB-09: the shadow of the draw
    ('nvn_nb_tap.inc', 'static int g_nbd_on;'),
    ('nvn_nb_tap.inc', 'static uint32_t g_nbd_texgen = 1;'),
    ('nvn_nb_tap.inc', 'static __thread struct { int in, emitting, drew; unsigned op; cmd_t *cb; nbd_shadow s; } t_nbd;'),
    ('nvn_nb_tap.inc', '#define NBD_TAKE(c) (__builtin_expect'),
    ('nvn_nb_tap.inc', '#define NBD_TEXGEN() do'),
    # NR-09R / NR-UBD, first part: the switches, the counters, the signature, the mirrors' hook lines
    ('nvn_nr_direct.inc', 'static int g_nrd_mode;'),
    ('nvn_nr_direct.inc', 'static int g_nrd_tap, g_nrd_texf;'),
    ('nvn_nr_direct.inc', 'static uint64_t g_nrdx[NVN_NRD_S_N];'),
    ('nvn_nr_direct.inc', 'static __thread uint64_t t_nrdx[NVN_NRD_S_N];'),
    ('nvn_nr_direct.inc', '#define NRD_ADD(k, n)'),
    ('nvn_nr_direct.inc', 'static void nrd_cnt_flush(void)'),
    ('nvn_nr_direct.inc', '#define NRD_TICK()'),
    ('nvn_nr_direct.inc', 'static uint64_t g_nrd_tabsig;'),
    ('nvn_nr_direct.inc', '#define NRD_SIG(w, st, s, was, now) do'),
    ('nvn_nr_direct.inc', 'static nrd_mirror g_nrdm;'),
    ('nvn_nr_direct.inc', 'static pthread_mutex_t g_nrdm_mu'),
    ('nvn_nr_direct.inc', 'static __thread uint32_t t_nrd_wf, t_nrd_nf;'),
    ('nvn_nr_direct.inc', 'static int nrdm_bind_hook(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h);'),
    ('nvn_nr_direct.inc', '#define NRD_BT(c, op, st, first, n, h) (__builtin_expect'),
    ('nvn_nr_direct.inc', '#define NRD_BT_FLAG(same) do'),
    ('nvn_nr_direct.inc', 'static int g_nrub;'),
    ('nvn_nr_direct.inc', 'static nrub_mirror g_nrub_m;'),
    ('nvn_nr_direct.inc', 'static pthread_mutex_t g_nrub_mu'),
    ('nvn_nr_direct.inc', 'static __thread int t_nrub_scope;'),
    ('nvn_nr_direct.inc', 'static int nrub_bind1_hook(NVNcommandBuffer *c, int st, int slot, uint64_t a, uint64_t sz);'),
    ('nvn_nr_direct.inc', '#define NRUB_BIND1(c, st, i, a, n) (__builtin_expect'),
    ('nvn_nr_direct.inc', '#define NRUB_FLAG(same) do'),
    ('nvn_nr_direct.inc', '#define NRD_WF_RESET() do'),
    ('nvn_nb_tap.inc', 'static void nbt_bind(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {'),
    # NB-09: a bind into the shadow
    ('nvn_nb_draw.inc', 'static void nbd_reset(void)'),
    ('nvn_nb_draw.inc', 'static __thread int t_nbd_chk, t_nbd_mirror;'),
    ('nvn_nb_draw.inc', 'static void nbd_cb(NVNcommandBuffer *c) {'),
    ('nvn_nb_draw.inc', 'static int nbd_take_bind(NVNcommandBuffer *c, unsigned kind, int stage, int slot, uint64_t a, uint64_t b) {'),
    # NR-09R / NR-UBD, second part: the mirrors' hooks, the gate and the texture bind of the direct recorder
    ('nvn_nr_direct.inc', 'static int nrdm_bind_hook(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {'),
    ('nvn_nr_direct.inc', 'static int nrub_bind_hook(NVNcommandBuffer *c, int st, int first, int n, const uint64_t *a, const uint64_t *sz, int filt) {'),
    ('nvn_nr_direct.inc', 'static int nrub_bind1_hook(NVNcommandBuffer *c, int st, int slot, uint64_t a, uint64_t sz) {'),
    ('nvn_nr_direct.inc', 'int nvn_nrd_can(uint64_t cb) {'),
    # the entry points themselves
    ('hle_nvn.c', 'NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tex, int samp)'),
    ('hle_nvn.c', 'NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {'),
    ('hle_nvn.c', 'static void bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {'),
    ('nvn_nr_direct.inc', 'void nvn_nrd_bind(uint64_t cb, int stage, int slot, int32_t tex, int32_t smp) {'),
]


def piece(text, start, name):
    lines = text.split('\n')
    hits = [i for i, l in enumerate(lines) if l.startswith(start)]
    if len(hits) != 1:
        sys.exit('dos2_nrnb_bodies: %d lines of %s start with %r (one expected)' % (len(hits), name, start))
    i = hits[0]
    if start.startswith('#define'):
        j = i
        while lines[j].rstrip().endswith('\\'):
            j += 1
        return '\n'.join(lines[i:j + 1])
    code = lines[i].split('//')[0]
    if '{' not in code and code.rstrip().endswith(';'):
        return lines[i]
    depth, seen, j = 0, False, i
    while True:
        line = re.sub(r'"(\\.|[^"\\])*"', '""', lines[j])
        line = re.sub(r"'(\\.|[^'\\])'", "''", line).split('//')[0]
        for ch in line:
            if ch == '{':
                depth += 1; seen = True
            elif ch == '}':
                depth -= 1
        if seen and depth == 0:
            return '\n'.join(lines[i:j + 1])
        j += 1
        if j >= len(lines):
            sys.exit('dos2_nrnb_bodies: no end of %r in %s' % (start, name))


def fnv(b):
    h = 0xcbf29ce484222325
    for c in b:
        h = ((h ^ c) * 0x100000001b3) & ((1 << 64) - 1)
    return h


def pieces():
    cache, out = {}, []
    for name, start in PIECES:
        if name not in cache:
            cache[name] = (ROOT / name).read_text(encoding='utf-8').replace('\r\n', '\n')
        out.append((name, start, piece(cache[name], start, name)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out')
    ap.add_argument('--hash', action='store_true')
    a = ap.parse_args()
    ps = pieces()
    if a.hash:
        for name, start, body in ps:
            print('%016x %s: %s' % (fnv(body.encode('utf-8')), name, start))
        return
    text = '// GENERATED by tools/dos2_nrnb_bodies.py from dm/gfx/nvn - do not edit, do not commit.\n'
    for name, start, body in ps:
        text += '// ---- %s ----\n%s\n' % (name, body)
    if a.out:
        pathlib.Path(a.out).write_text(text, encoding='utf-8', newline='\n')
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
