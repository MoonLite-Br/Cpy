class Point:
    dims = 2
    def __init__(self, x, y=0):
        self.x = x
        self.y = y
    def __str__(self):
        return f"({self.x}, {self.y})"
    def __repr__(self):
        return "Point(%d, %d)" % (self.x, self.y)
    def __add__(self, o):
        return Point(self.x + o.x, self.y + o.y)
    def __eq__(self, o):
        return self.x == o.x and self.y == o.y
    def __lt__(self, o):
        return (self.x, self.y) < (o.x, o.y)
    def __len__(self):
        return 2
    def norm2(self):
        return self.x ** 2 + self.y ** 2
p = Point(1, 2)
q = Point(3)
print(p, q, p + q, repr(p), [p, q], p == Point(1, 2), p != q, p < q, q > p, len(p), Point.dims, p.dims)
print(p.norm2(), Point.norm2(q), sorted([q, p]), max(p, q))
p.x = 10
p.extra = "hi"
print(p, p.extra, hasattr(p, "x"), hasattr(p, "nope"), getattr(p, "y"), getattr(p, "nope", "dflt"))
setattr(p, "z", 3)
print(p.z, isinstance(p, Point), isinstance(3, Point), type(p) == Point, type(p).__name__)
class Animal:
    def __init__(self, name):
        self.name = name
    def speak(self):
        return "..."
    def intro(self):
        return self.name + " says " + self.speak()
class Dog(Animal):
    def __init__(self, name, tricks=0):
        super().__init__(name)
        self.tricks = tricks
    def speak(self):
        return "Woof"
class Puppy(Dog):
    def speak(self):
        return super().speak() + "!"
for a in [Animal("Generic"), Dog("Rex"), Puppy("Bit", 2)]:
    print(a.intro(), a.__class__.__name__ if False else type(a).__name__)
print(isinstance(Puppy("a"), Animal), isinstance(Animal("a"), Dog), Puppy("z").tricks)
class Counter:
    count = 0
    def __init__(self):
        Counter.count += 1
c1 = Counter(); c2 = Counter()
print(Counter.count, c1.count)
class Stack:
    def __init__(self):
        self.items = []
    def push(self, v):
        self.items.append(v)
        return self
    def pop(self):
        return self.items.pop()
    def __len__(self):
        return len(self.items)
    def __getitem__(self, i):
        return self.items[i]
    def __contains__(self, v):
        return v in self.items
    def __bool__(self):
        return bool(self.items)
s = Stack().push(1).push(2).push(3)
print(len(s), s[0], s[-1], 2 in s, 9 in s, bool(s), bool(Stack()), s.pop(), len(s))
class Temp:
    def __init__(self, c):
        self.c = c
    def __call__(self, k):
        return self.c * k
print(Temp(3)(4), callable(Temp(1)), callable(len), callable(5))
class Empty:
    pass
e = Empty()
e.a = 1
print(e.a, str(type(e).__name__))
class Node:
    def __init__(self, val, nxt=None):
        self.val = val
        self.nxt = nxt
head = Node(1, Node(2, Node(3)))
cur = head
vals = []
while cur:
    vals.append(cur.val)
    cur = cur.nxt
print(vals)
