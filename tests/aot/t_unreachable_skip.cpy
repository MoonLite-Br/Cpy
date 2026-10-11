class UnusedHelper:
    def __init__(self, x):
        self.x = x
    def compute(self):
        return self.x * 2

def unused_string_fn():
    return "this uses a string, AOT can't compile it"

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(15))
