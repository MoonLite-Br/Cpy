def g(n):
    try:
        for i in range(n):
            yield i
    finally:
        pass
gens = [g(10) for _ in range(3000)]
t = 0
for x in gens:
    t += next(x) + next(x)
print(t)
for x in gens:
    x.close()
print("closed")
def bad():
    yield 1
    x = 1 // 0
h = bad()
next(h)
try:
    next(h)
except ZeroDivisionError:
    print("zde")
def deep(n):
    if n == 0:
        yield 0
    else:
        for v in deep(n - 1):
            yield v + 1
print(list(deep(50)))
