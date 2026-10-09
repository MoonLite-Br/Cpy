# --- keyword arguments, *args/**kwargs, keyword-only
def f(a, b=2, *args, c=3, d, **kw):
    return (a, b, args, c, d, sorted(kw.items()))
print(f(1, d=4))
print(f(1, 2, 3, 4, d=5, c=6, x=7, y=8))
def g(*, k=1):
    return k
print(g(), g(k=5))
def h(**kw):
    return kw
print(h(a=1, b=2), h(**{"x": 1}), h(**{"a": 1}, b=2))
def pos(a, b, /, c):
    return a + b + c
print(pos(1, 2, 3), pos(1, 2, c=3))
def outer(*a, **k):
    return inner(*a, **k)
def inner(x, y=0, z=0):
    return x + y + z
print(outer(1), outer(1, 2), outer(1, z=5), outer(*[1, 2], **{"z": 3}))
for bad in [lambda: f(), lambda: f(1), lambda: g(1), lambda: inner(), lambda: inner(1, q=2), lambda: inner(1, 2, 3, 4), lambda: inner(1, x=2)]:
    try:
        bad()
    except TypeError as e:
        print(e)
# --- star unpacking
a, *b = [1, 2, 3, 4]
*c, d = "wxyz"
e, *m, z = range(6)
print(a, b, c, d, e, m, z)
first, *_ = (9, 8, 7)
print(first, _)
for i, *rest in [(1, 2, 3), (4, 5)]:
    print(i, rest)
print([*range(3), *"ab"], (*[1, 2], 3), {*[1, 2, 2]} == {1, 2}, {**{"a": 1}, "b": 2, **{"a": 3}})
print(*[1, 2, 3], sep="-")
# --- walrus
if (n := len("hello")) > 3:
    print("long", n)
data = [1, 2, 3, 4, 5, 6]
print([y for x in data if (y := x * x) > 10])
while (n := n - 1) > 2:
    print("n =", n)
# --- decorators, static/class methods, properties
def deco(fn):
    def wrapper(*args, **kwargs):
        print("calling", fn.__name__)
        return fn(*args, **kwargs)
    return wrapper
def repeat(times):
    def dec(fn):
        def wrapper(*a):
            return [fn(*a) for _ in range(times)]
        return wrapper
    return dec
@deco
def add(x, y):
    return x + y
@repeat(3)
def hi(name):
    return "hi " + name
print(add(1, 2), hi("bob"))
class Circle:
    count = 0
    def __init__(self, r):
        self._r = r
        Circle.count += 1
    @property
    def radius(self):
        return self._r
    @radius.setter
    def radius(self, v):
        if v < 0:
            raise ValueError("negative")
        self._r = v
    @property
    def area(self):
        return 3 * self._r ** 2
    @staticmethod
    def unit():
        return Circle(1)
    @classmethod
    def make(cls, r):
        return cls(r * 2)
    def __repr__(self):
        return f"Circle({self._r})"
c = Circle(2)
c.radius = 5
print(c.radius, c.area, Circle.unit(), Circle.make(3), Circle.count, c)
try:
    c.radius = -1
except ValueError as e:
    print("error:", e)
try:
    c.area = 1
except AttributeError:
    print("read-only")
# --- multiple inheritance and MRO
class A:
    def who(self): return "A"
class B(A):
    def who(self): return "B" + super().who()
class C(A):
    def who(self): return "C" + super().who()
class D(B, C):
    def who(self): return "D" + super().who()
print(D().who(), [k.__name__ for k in D.__mro__], isinstance(D(), C), issubclass(D, A), issubclass(B, C))
class Base:
    def __init__(self, x):
        self.x = x
class Mixin:
    def show(self):
        return f"<{self.x}>"
class Both(Mixin, Base):
    pass
print(Both(5).show())
# --- __getattr__ / __setattr__ / __eq__+__hash__
class Proxy:
    def __init__(self):
        object.__setattr__(self, "log", [])
    def __getattr__(self, name):
        return "missing:" + name
    def __setattr__(self, name, value):
        self.log.append(name)
        object.__setattr__(self, name, value)
p = Proxy()
p.a = 1
print(p.a, p.b, p.log)
class Pt:
    def __init__(self, x): self.x = x
    def __eq__(self, o): return self.x == o.x
    def __hash__(self): return hash(self.x)
print(len({Pt(1), Pt(1), Pt(2)}), {Pt(1): "a"}[Pt(1)])
# --- misc syntax
def stub(): ...
print(stub(), ...)
x = 5
print(f"{x=}", f"{x + 1 = }", f"{x=:>4}")
with open("tmp_t10.txt", "w") as f1, open("tmp_t10b.txt", "w") as f2:
    f1.write("a"); f2.write("b")
import os
os.remove("tmp_t10.txt"); os.remove("tmp_t10b.txt")
class Ann:
    x: int = 5
    y: str
    def m(self) -> int:
        return self.x
print(Ann().m(), Ann.__annotations__ if False else "ann ok")
def typed(a: int, b: "str" = "x", *args: int, **kw: str) -> None:
    return (a, b, args, kw)
print(typed(1, "y", 2, k="v"))
