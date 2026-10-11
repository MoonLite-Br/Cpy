def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(fib(15))

def outer(x):
    def middle(y):
        def inner(z):
            return x + y + z
        return inner
    return middle

print(outer(1)(2)(3))

class Stack:
    def __init__(self):
        self.items = []
    def push(self, v):
        self.items.append(v)
    def pop(self):
        n = len(self.items)
        v = self.items[n - 1]
        self.items = self.items[0:n - 1]
        return v
    def size(self):
        return len(self.items)

s = Stack()
s.push(1)
s.push(2)
s.push(3)
print(s.size(), s.pop(), s.pop(), s.size())

count = 0
def bump():
    global count
    count = count + 1

bump()
bump()
bump()
print(count)

matrix = [[1, 2], [3, 4]]
print(matrix[0][1], matrix[1][0])
total = 0
for row in matrix:
    for v in row:
        total = total + v
print(total)
