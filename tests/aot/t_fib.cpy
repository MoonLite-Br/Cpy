def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(20))
print(fib(0))
print(fib(1))
