def f(n):
    t = 0
    i = 0
    while i < n:
        t = t + i * 2 % 7
        i += 1
    return t
print(f(3000000))
def g(n):
    s = 0
    for i in range(n):
        for j in range(10):
            if (i + j) % 3 == 0:
                s += j
    return s
print(g(300000))
