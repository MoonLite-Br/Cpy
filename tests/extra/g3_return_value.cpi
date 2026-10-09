def gen():
    yield 1
    yield 2
    return 99

g = gen()
print(next(g))
print(next(g))
try:
    next(g)
except StopIteration as e:
    print("stopped with", e.value)
