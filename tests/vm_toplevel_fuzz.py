"""Differential fuzzer for the module-/script-level VM tier (vm_compile_toplevel
in src/vm.c). Generates random TOP-LEVEL programs (no wrapping function) mixing
plain statements, loops, comprehensions, try/except, and -- deliberately, since
that's the one thing that must make the whole file fall back to the tree-walker
-- occasional nested def/class, and checks tree-walker (--no-vm) output against
default (VM) output are identical.

Usage: python3 tests/vm_toplevel_fuzz.py N [SEED0]
"""
import os
import random
import subprocess
import sys

BIN = os.environ.get('CPY', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bin', 'cpy'))


def gen(seed):
    R = random.Random(seed)
    names = ['a', 'b', 'c', 'd']

    def atom():
        r = R.random()
        if r < 0.35: return R.choice(names)
        if r < 0.55: return str(R.randint(-5, 20))
        if r < 0.62: return repr(round(R.uniform(-3, 9), 2))
        if r < 0.68: return R.choice(['True', 'False', 'None'])
        return "'%s'" % R.choice(['x', 'y', 'ab'])

    def expr(d=0):
        if d >= 2 or R.random() < 0.3: return atom()
        k = R.random()
        if k < 0.4: return '(%s %s %s)' % (expr(d + 1), R.choice(['+', '-', '*']), expr(d + 1))
        if k < 0.6: return '(%s %s %s)' % (expr(d + 1), R.choice(['<', '<=', '>', '==', '!=']), expr(d + 1))
        if k < 0.75: return '(%s if %s else %s)' % (expr(d + 1), expr(d + 1), expr(d + 1))
        return '[%s for _x in range(4)]' % expr(d + 1)

    def stmt(ind, inloop):
        p = ' ' * ind
        k = R.random()
        if k < 0.35: return [p + '%s = %s' % (R.choice(names), expr())]
        if k < 0.45: return [p + 'out.append(%s)' % expr()]
        if k < 0.55:
            r = [p + 'if %s:' % expr()] + block(ind + 2, inloop)
            if R.random() < 0.5: r += [p + 'else:'] + block(ind + 2, inloop)
            return r
        if k < 0.68:
            r = [p + 'for _i in range(%d):' % R.randint(0, 5)] + block(ind + 2, True)
            if R.random() < 0.3: r += [p + 'else:'] + block(ind + 2, inloop)
            return r
        if k < 0.75 and inloop: return [p + R.choice(['break', 'continue'])]
        if k < 0.85:
            return [p + 'try:'] + block(ind + 2, inloop) + [p + 'except Exception as e:', p + '  out.append(type(e).__name__)']
        if k < 0.90:
            # nested def -- forces the whole file onto the tree-walker; must still match
            return [p + 'def _h(v=%s):' % expr(), p + '  return v', p + 'out.append(_h())']
        return [p + 'out.append(%s)' % expr()]

    def block(ind, inloop):
        n = R.randint(1, 3)
        r = []
        for _ in range(n): r += stmt(ind, inloop)
        return r or [' ' * ind + 'pass']

    lines = ['out = []', 'a = %s' % atom(), 'b = 1', 'c = 2.0', 'd = "z"']
    lines += block(0, False)
    lines += ['print(out, a, b, c, d)']
    return '\n'.join(lines) + '\n'


def main():
    n = int(sys.argv[1])
    s0 = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    bad = 0
    for seed in range(s0, s0 + n):
        src = gen(seed)
        path = '/tmp/cpy_toplevel_fz.cpy'
        open(path, 'w').write(src)
        try:
            a = subprocess.run([BIN, '--no-vm', path], capture_output=True, text=True, timeout=10)
            b = subprocess.run([BIN, path], capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired:
            print('timeout seed', seed)
            continue
        if (a.stdout, a.stderr, a.returncode) != (b.stdout, b.stderr, b.returncode):
            bad += 1
            open('/tmp/cpy_toplevel_bad_%d.cpy' % seed, 'w').write(src)
            print('MISMATCH seed', seed)
            print(' tw:', (a.stdout + a.stderr)[:200])
            print(' vm:', (b.stdout + b.stderr)[:200])
            if bad >= 5: break
    print('done, mismatches:', bad)


if __name__ == '__main__':
    main()
