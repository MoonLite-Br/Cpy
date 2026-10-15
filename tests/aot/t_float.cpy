def avg(a, b):
    return (a + b) / 2

def powsum(x, n):
    total = 0.0
    i = 0
    while i < n:
        total = total + x
        i = i + 1
    return total

print(avg(3, 4))
print(avg(3.5, 4.5))
print(powsum(1.5, 4))
