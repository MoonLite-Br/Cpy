def g(n):
    for i in range(n):
        yield i
t = 0
for k in range(2000):
    for x in g(5):
        t += x
print(t)
