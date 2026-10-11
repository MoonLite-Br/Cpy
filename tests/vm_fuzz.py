import random, subprocess, sys
import os
BIN=os.environ.get('CPY', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bin', 'cpy'))
def gen(seed):
    R=random.Random(seed)
    names=['a','b','c','d']
    def atom(depth):
        r=R.random()
        if r<0.30: return R.choice(names)
        if r<0.50: return str(R.randint(-5,20))
        if r<0.58: return repr(round(R.uniform(-3,9),2))
        if r<0.62: return R.choice(['True','False','None'])
        if r<0.68: return 'L[%s %% 4]' % R.choice(names)
        return R.choice(names)
    def expr(d=0):
        if d>=3 or R.random()<0.25: return atom(d)
        k=R.random()
        if k<0.45:
            return '(%s %s %s)' % (expr(d+1), R.choice(['+','-','*','//','%','/','**']), expr(d+1))
        if k<0.60:
            return '(%s %s %s)' % (expr(d+1), R.choice(['<','<=','>','>=','==','!=']), expr(d+1))
        if k<0.70: return '(%s %s %s)' % (expr(d+1), R.choice(['and','or']), expr(d+1))
        if k<0.75: return '(not %s)' % expr(d+1)
        if k<0.80: return '(-%s)' % expr(d+1)
        if k<0.86: return '(%s if %s else %s)' % (expr(d+1), expr(d+1), expr(d+1))
        if k<0.92: return 'h(%s, %s)' % (expr(d+1), expr(d+1))
        return 'abs(%s)' % expr(d+1)
    def stmt(ind, d, inloop):
        p=' '*ind
        k=R.random()
        if k<0.30: return [p+'%s = %s' % (R.choice(names), expr())]
        if k<0.42: return [p+'%s %s= %s' % (R.choice(names), R.choice(['+','-','*','//','%']), expr())]
        if k<0.48: return [p+'L[%s %% 4] = %s' % (R.choice(names), expr())]
        if k<0.52: return [p+'out.append(%s)' % expr()]
        if d>=2: return [p+'out.append(%s)' % R.choice(names)]
        if k<0.66:
            r=[p+'if %s:' % expr()]+block(ind+2,d+1,inloop)
            if R.random()<0.5: r+= [p+'else:']+block(ind+2,d+1,inloop)
            return r
        if k<0.80:
            v=R.choice(['i','j'])
            r=[p+'for %s in range(%d, %d%s):' % (v, R.randint(-2,3), R.randint(0,6), R.choice(['',', 2',', -1']))]+block(ind+2,d+1,True)
            if R.random()<0.25: r+=[p+'else:']+block(ind+2,d+1,inloop)
            return r
        if k<0.86:
            return [p+'w = 0', p+'while w < %d and %s:' % (R.randint(0,5), expr()), p+'  w += 1']+block(ind+2,d+1,True)
        if k<0.90 and inloop: return [p+R.choice(['break','continue'])]
        if k<0.94: return [p+'try:']+block(ind+2,d+1,inloop)+[p+'except Exception as e:', p+'  out.append(type(e).__name__)']
        return [p+'out.append(%s)' % expr()]
    def block(ind,d,inloop):
        n=R.randint(1,3); r=[]
        for _ in range(n): r+=stmt(ind,d,inloop)
        return r
    lines=['def h(x, y):', '  if x is None or y is None:', '    return 0', '  return x', 
           'def f(a, b):', '  c = %s' % atom(0), '  d = 1', '  L = [1, 2, 3, 4]', '  out = []']
    lines+=block(2,0,False)
    lines+=['  return out, a, b, c, d, L']
    lines+=['def run(a, b):','  try:','    return f(a, b)','  except Exception as e:','    return type(e).__name__',
            'for p in [(1,2),(0,0),(7,-3),(2.5,4),(-4,9)]:','  print(run(p[0], p[1]))']
    return '\n'.join(lines)+'\n'
bad=0; n=int(sys.argv[1]); s0=int(sys.argv[2]) if len(sys.argv)>2 else 0
for seed in range(s0, s0+n):
    src=gen(seed); open('/tmp/cpy_vm_fuzz.cpy','w').write(src)
    try:
        a=subprocess.run([BIN,'--no-vm','/tmp/cpy_vm_fuzz.cpy'],capture_output=True,text=True,timeout=20)
        b=subprocess.run([BIN,'/tmp/cpy_vm_fuzz.cpy'],capture_output=True,text=True,timeout=20)
    except subprocess.TimeoutExpired:
        print('timeout seed',seed); continue
    if (a.stdout,a.stderr,a.returncode)!=(b.stdout,b.stderr,b.returncode):
        bad+=1; open('/tmp/cpy_vm_bad_%d.cpy'%seed,'w').write(src)
        print('MISMATCH seed',seed); print(' tw:',(a.stdout+a.stderr)[:200]); print(' vm:',(b.stdout+b.stderr)[:200])
        if bad>=3: break
print('done, mismatches:',bad)
