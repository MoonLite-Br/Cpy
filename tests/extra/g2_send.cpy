def echo():
    total = 0
    while True:
        x = yield total
        if x is None:
            break
        total += x

g = echo()
print(next(g))
print(g.send(5))
print(g.send(10))
