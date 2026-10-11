def inner(n):
    for i in range(n):
        yield i * i

def outer(n):
    for v in inner(n):
        yield v + 1000

print(list(outer(5)))
