def f(n):
    if n < 0:
        raise ValueError("neg")
    return n
try:
    f(-1)
except ValueError as e:
    print("caught", e)
