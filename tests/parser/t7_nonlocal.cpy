def outer():
    n = 0
    def inc():
        nonlocal n
        n += 1
        return n
    return inc
f = outer()
print(f(), f(), f())
