def gen():
    try:
        yield 1
        yield 2
    finally:
        print("cleanup ran")

g = gen()
print(next(g))
g.close()
print("closed ok")
