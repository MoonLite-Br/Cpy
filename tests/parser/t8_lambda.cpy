f = lambda x: x + 1
print(f(5))
g = lambda x, y=10: x + y
print(g(1), g(1, 2))
print(sorted([3, 1, 2], key=lambda x: -x))
