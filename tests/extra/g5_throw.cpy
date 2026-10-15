def gen():
    try:
        yield 1
        yield 2
    except ValueError as e:
        print("caught inside:", e)
        yield 99

g = gen()
print(next(g))
print(g.throw(ValueError("boom")))
