# Deliberately boxed (the `** 1` disqualifies the fast/unboxed tier) so this
# is a permanent regression test for FULL frame-for-frame AOT tracebacks.
def inner(n):
    n = n ** 1
    return n // 0

def outer(n):
    n = n ** 1
    return inner(n) + 1

print(outer(5))
