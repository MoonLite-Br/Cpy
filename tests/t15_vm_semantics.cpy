def unbound():
    try:
        print(x)
    except UnboundLocalError as e:
        print("UL", e)
    x = 1
    return x
print(unbound())

def unbound2(c):
    if c:
        y = 5
    return y + 1
print(unbound2(True))
try:
    unbound2(False)
except UnboundLocalError as e:
    print("UL2", e)

def deleted():
    a = 3
    del a
    try:
        return a + 1
    except UnboundLocalError as e:
        return "del ok"
print(deleted())

def closure():
    n = 0
    def inc():
        nonlocal n
        n += 1
        return n
    a = n + inc()
    b = n
    return a, b, inc(), n
print(closure())

def walrus(xs):
    t = 0
    for x in xs:
        if (y := x * 2) > 4:
            t += y
    return t, y
print(walrus([1, 2, 3, 4]))

def loops():
    out = []
    for i in range(10):
        if i == 2:
            continue
        if i == 6:
            break
        try:
            if i == 4:
                continue
            out.append(i)
        finally:
            out.append(-i)
    else:
        out.append("else")
    j = 0
    while j < 5:
        j += 1
        if j == 3:
            break
    else:
        out.append("wh-else")
    k = 0
    while k < 3:
        k += 1
    else:
        out.append("wh-else2")
    return out, j, k
print(loops())

def strs():
    s = ""
    for i in range(5):
        s += str(i)
    t = s
    s += "x"
    u = s
    s += s
    return s, t, u
print(strs())

def alias():
    a = [1, 2]
    b = a
    b += [3]
    c = a
    a = a + [4]
    return a, b, c
print(alias())

def swap():
    a, b = 1, 2
    a, b = b, a
    x = [0, 0]
    x[0], x[1] = 5, 6
    i = 0
    i, x[i] = 1, 9
    return a, b, x, i
print(swap())

def chain(a):
    return 1 < a < 5, a and 7, a or 8, not a, (a if a else -1)
print(chain(0), chain(3))

def andor(a, b):
    a = a and b
    b = b or a
    return a, b
print(andor(0, 5), andor(2, 0), andor("", "x"))

def rng():
    r = []
    for i in range(10, 0, -3):
        r.append(i)
    for i in range(0):
        r.append("never")
    for a, b in [(1, 2), (3, 4)]:
        r.append(a + b)
    for k in {"a": 1}:
        r.append(k)
    for ch in "hi":
        r.append(ch)
    return r
print(rng())

class P:
    def __init__(self, v):
        self.v = v
    def add(self, o):
        return P(self.v + o.v)
    def __repr__(self):
        return "P(%d)" % self.v
    def m(self, x, y=2):
        return self.v * x + y
def oop():
    a = P(1)
    b = a.add(P(2)).add(P(3))
    return b, b.m(3), b.m(3, y=1), b.v
print(oop())

def kw(a, b=2, *args, **kws):
    return a, b, args, kws
def callit():
    return kw(1), kw(1, 3, 4, 5), kw(1, z=9), kw(*[1, 2, 3], **{"q": 1})
print(callit())

def flt():
    x = 1
    x /= 2
    y = 7 // 2 + 7 % 3 - 2 ** 3
    z = -x
    w = -y
    return x, y, z, w, 10 / 4, 3 * 1.5, 1 + True, 2 ** -1
print(flt())

def bigint():
    a = 9223372036854775807
    return a + 1, a * 2, -a - 2
try:
    print(bigint())
except OverflowError as e:
    print("OE", e)

def gen_user():
    def g(n):
        for i in range(n):
            yield i * i
    s = 0
    for v in g(5):
        s += v
    return s, list(g(3))
print(gen_user())

def rec(n):
    return 0 if n == 0 else 1 + rec(n - 1)
print(rec(500))
try:
    rec(100000)
except RecursionError as e:
    print("RE", e)

def exc():
    try:
        return 1 // 0
    except ZeroDivisionError as e:
        return "zde"
    finally:
        pass
print(exc())
def glob():
    global G
    G = 5
    G += 1
    return G
print(glob(), G)

def default_mut(a, l=[]):
    l.append(a)
    return l
print(default_mut(1), default_mut(2))
