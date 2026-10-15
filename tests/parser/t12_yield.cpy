def gen():
    yield 1
    yield 2
    x = yield 3
    yield x
def gen2():
    yield from [10, 20, 30]
print(list(gen2()))
g = gen()
print(next(g))
print(next(g))
print(next(g))
print(g.send(99))
