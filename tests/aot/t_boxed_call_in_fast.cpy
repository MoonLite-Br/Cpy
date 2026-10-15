def helper_boxed(n):
    return (n ** 1) + 1

def f(n):
    print(n)
    print(abs(n - 10))
    helper_boxed(n)   # bare statement, result truly discarded -- should still allow fast tier
    return n * 2

def g(n):
    return abs(n) + 1  # builtin result USED -- must stay boxed

def not_fast_because_captured(n):
    x = helper_boxed(n)  # captured even though unused after -- must stay boxed (no dead-code elim)
    return n

def h():
    print()
    return 42

print(f(3))
print(f(15))
print(f(-5))
print(g(-7))
print(not_fast_because_captured(9))
print(h())
