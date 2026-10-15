def sieve(n):
    flags = [True] * (n + 1)
    flags[0] = False
    flags[1] = False
    i = 2
    while i * i <= n:
        if flags[i]:
            j = i * i
            while j <= n:
                flags[j] = False
                j += i
        i += 1
    c = 0
    for x in flags:
        if x:
            c += 1
    return c
print(sieve(2000000))
def collatz(n):
    steps = 0
    while n != 1:
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
        steps += 1
    return steps
best = 0
for k in range(1, 30000):
    c = collatz(k)
    if c > best:
        best = c
print(best)
