def gen():
    yield 1
    raise ValueError("inner error")

g = gen()
print(next(g))
try:
    next(g)
except ValueError as e:
    print("propagated:", e)
