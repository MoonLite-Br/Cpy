def add(a, b=10):
    return a + b

print(add(1), add(1, 2))

def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)

print(fact(10))

def make_counter():
    count = [0]
    def inc():
        count[0] = count[0] + 1
        return count[0]
    return inc

c = make_counter()
print(c(), c(), c())
