def sumrange(n):
    total = 0
    for i in range(n):
        total += i
    return total

def gcd(a, b):
    while b != 0:
        r = a % b
        a = b
        b = r
    return a

print(sumrange(100))
print(sumrange(0))
print(gcd(48, 18))
print(gcd(17, 5))
