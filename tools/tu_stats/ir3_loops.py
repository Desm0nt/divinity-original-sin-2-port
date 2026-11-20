"""N1: issue-cycle model of an ir3 disassembly (IR3_SHADER_DEBUG=disasm output of tu_stats): total issue slots and, per loop (a branch
back to an earlier label), the slots of its body. A line costs 1 + its (rptN) count (nop included: Adreno issues nops in order).
A wave with mixed lanes runs both sides of a branch, so a loop body here = every instruction between the label and the back edge.
  ir3_loops.py <disasm file> [--stage FRAGMENT|VERTEX] [--which -1]   (the which-th shader of that stage in the file, default the last)
"""
import re, sys
def shaders(lines, stage):
    out, cur = [], None
    for l in lines:
        l = l.replace('MESA: info: ', '', 1).rstrip('\n')
        if l.startswith('shader: MESA_SHADER_'):
            cur = [] if l.endswith(stage) else None
            if cur is not None: out.append(cur)
            continue
        if cur is not None: cur.append(l)
    return out
INS = re.compile(r'^\s*(\([^)]*\))*\s*[a-z]')
def cost(l):
    m = re.search(r'\(rpt(\d)\)', l); return 1 + (int(m.group(1)) if m else 0)
def analyse(body):
    code, labels = [], {}
    started = False
    for l in body:
        s = l.strip()
        if re.match(r'^l\d+:$', s): labels[s[:-1]] = len(code); started = True; continue
        if not started and not s.startswith(('mov', '(', 'nop', 'mad', 'add', 'mul', 'sam', 'isam', 'ldc', 'br', 'jump', 'bary', 'flat', 'end', 'cmps', 'sel', 'shps', 'getone', 'ldg', 'stg')): continue
        if INS.match(s) and not s.startswith(('shader', 'source', 'inputs', 'outputs', 'decl', 'impl', 'con', 'div', 'vec', 'block', 'if', 'else', 'loop', 'break', 'continue')):
            started = True; code.append(s)
    total = sum(cost(c) for c in code)
    loops = []
    for i, c in enumerate(code):
        m = re.search(r'(?:jump|br)[^#]*#(l\d+)', c)
        if m and m.group(1) in labels and labels[m.group(1)] <= i:
            j = labels[m.group(1)]; loops.append((j, i, sum(cost(x) for x in code[j:i + 1]), sum(1 for x in code[j:i + 1] if 'nop' in x)))
    return total, len(code), loops
if __name__ == '__main__':
    a = sys.argv[1:]; stage = 'FRAGMENT'; which = -1
    if '--stage' in a: stage = a[a.index('--stage') + 1]
    if '--which' in a: which = int(a[a.index('--which') + 1])
    sh = shaders(open(a[0], errors='replace').readlines(), stage)
    total, n, loops = analyse(sh[which])
    print('%s: %d instructions, %d issue slots; loops (start, end, body slots, nop lines): %s' % (a[0], n, total, loops))
