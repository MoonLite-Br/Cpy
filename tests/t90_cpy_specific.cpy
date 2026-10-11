# Behaviour specific to cpy (checked by hand, not compared with CPython).
# 1. integers are 64-bit: overflow raises OverflowError instead of growing
def check(name, f):
    try:
        f()
        print(name, "no error")
    except OverflowError:
        print(name, "-> OverflowError")
check("2 ** 63", lambda: 2 ** 63)
check("max + 1", lambda: 9223372036854775807 + 1)
check("min - 1", lambda: -9223372036854775807 - 2)
check("2**62 * 2", lambda: 4611686018427387904 * 2)
check("1 << 63", lambda: 1 << 63)
check("2 ** 62", lambda: 2 ** 62)
def big():
    return 2 ** 64
try:
    big()
except OverflowError as e:
    print("OverflowError:", e)
try:
    print(9223372036854775807 + 1)
except OverflowError:
    print("add overflow")
try:
    x = 3037000500 * 3037000500
except OverflowError:
    print("mul overflow")
print(2 ** 62 + (2 ** 62 - 1))
# 2. error messages
def show(f):
    try:
        f()
    except Exception as e:
        print(type(e).__name__ + ":", e)
show(lambda: 1 / 0)
show(lambda: [][0])
show(lambda: {}["k"])
show(lambda: nope)
show(lambda: "a" + 1)
show(lambda: len(5))
show(lambda: int("x"))
show(lambda: (1).foo)
show(lambda: undefined_fn())
show(lambda: [1, 2, 3].nope())
show(lambda: max([]))
show(lambda: "abc".index("z"))
show(lambda: dict([1]))
show(lambda: (lambda a, b: a)(1))
show(lambda: (lambda a: a)(1, 2))
show(lambda: (lambda a: a)(b=1))
show(lambda: float("1.2.3"))
show(lambda: [1, 2].index(9))
show(lambda: 5 < "a")
show(lambda: None + 1)
show(lambda: {[1]: 2})
show(lambda: "x".join([1]))
show(lambda: 5())
show(lambda: chr(-1))
# 3. tuples/lists are separate types
print([1] == (1,), type([]) == type(()), [1, 2] + [3], (1,) + (2,))
# 4. builtin type names
print(type(1), type("s"), type([]), type(None), type(len), type(show))
print(type(1).__name__, type(None).__name__)
# 5. slices
a = list(range(10))
a[2:5] = ["x"]
print(a)
a[::2] = a[::2] if False else a[::2]
del a[1:3]
print(a)
del a[::2]
print(a)
# 6. __name__ and argv
print(__name__)
import sys
print(len(sys.argv))
# 7. global statement at module level is harmless, while/for else
for i in range(3):
    pass
else:
    print("for-else ran", i)
n = 0
while n < 3:
    n += 1
    if n == 10:
        break
else:
    print("while-else ran", n)
for i in range(5):
    if i == 2:
        break
else:
    print("not printed")
print("done")
