class Shadow:
    def greet(self):
        return "class method"
a = Shadow()
b = Shadow()
b.greet = lambda: "shadowed!"
print(a.greet(), b.greet())
for i in range(3):
    print(a.greet(), b.greet())
lst = []
for i in range(1000):
    lst.append(i)
print(len(lst), sum(lst))
items = [[1], {1: 1}, [2, 3]]
for x in items:
    if isinstance(x, list):
        x.append(99)
    else:
        x[2] = 99
print(items)
s = "hello world"
for _ in range(3):
    print(s.upper(), s.split(" "))
