def add(a, b=10, *rest):
    return a + b + sum(rest)
print(add(1), add(1, 2), add(1, 2, 3, 4), add(b=5, a=1))
def kw(x, y=2, z=3):
    return (x, y, z)
print(kw(1), kw(1, z=9), kw(z=1, x=0), kw(*[7, 8]))
def fact(n):
    return 1 if n <= 1 else n * fact(n - 1)
print(fact(10), fact(20))
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
print([fib(i) for i in range(15)])
g = 100
def noshadow():
    g = 1
    return g
def useglobal():
    global g
    g += 1
    return g
print(noshadow(), g, useglobal(), g)
def outer():
    count = 0
    def inc():
        nonlocal count
        count += 1
        return count
    return inc
c = outer()
print(c(), c(), c())
def make_adder(n):
    return lambda x: x + n
add5 = make_adder(5)
print(add5(1), (lambda a, b: a * b)(3, 4), (lambda: 42)())
def compose(f, g):
    return lambda x: f(g(x))
print(compose(lambda x: x + 1, lambda x: x * 2)(5))
print(list(map(str, [1, 2])), sorted([(2, "b"), (1, "z")], key=lambda p: p[1]))
def default_list(x, acc=None):
    if acc is None:
        acc = []
    acc.append(x)
    return acc
print(default_list(1), default_list(2))
def noret():
    pass
print(noret())
def multi():
    return 1, "two", [3]
a, b, c = multi()
print(a, b, c, multi())
def tail(n, acc=0):
    if n == 0:
        return acc
    return tail(n - 1, acc + n)
print(tail(500))
def rec_depth(n):
    return 0 if n == 0 else 1 + rec_depth(n - 1)
print(rec_depth(900))
