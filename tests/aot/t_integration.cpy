class UnusedDebugHelper:
    def show(self):
        return "debug only, never called"

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def powsum(n):
    total = 0
    i = 0
    while i < n:
        print(i)
        total += i ** 2
        i += 1
    return total

print(fib(10))
print(powsum(3))
