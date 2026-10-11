def gen(n):
    i = 0
    while i < n:
        yield i
        i += 1
total = 0
for x in gen(100000):
    total += x
print(total)
