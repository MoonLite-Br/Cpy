def gen(n):
    for i in range(n):
        yield i * 2
def use(n):
    t = 0
    for x in gen(n):
        if x % 3 == 0:
            t += x
    return t
print(use(200000))
