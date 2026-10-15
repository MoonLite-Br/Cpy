def f(n):
    assert n > 0, "must be positive"
    return n * 2
print(f(5))
try:
    f(-1)
except AssertionError as e:
    print("assert failed:", e)
