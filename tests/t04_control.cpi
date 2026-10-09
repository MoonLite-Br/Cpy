for i in range(3):
    print(i, end=" ")
print()
i = 0
while i < 10:
    i += 1
    if i % 2 == 0:
        continue
    if i > 7:
        break
    print(i, end=",")
print()
for x in [1, 2, 3]:
    if x == 2:
        continue
    print(x)
else_hit = False
n = 15
if n < 10:
    print("small")
elif n < 20:
    print("medium")
elif n < 30:
    print("large")
else:
    print("huge")
total = 0
for i in range(1, 4):
    for j in range(1, 4):
        if j > i:
            break
        total += i * j
print(total)
for a, b in [(1, 2), (3, 4)]:
    print(a + b)
for k in {"x": 1, "y": 2}:
    print(k)
for ch in "hi":
    print(ch)
def early(n):
    for i in range(10):
        if i == n:
            return i
    return -1
print(early(3), early(20))
def first_neg(xs):
    for x in xs:
        while True:
            if x < 0:
                return x
            break
    return None
print(first_neg([1, 2, -5, 3]), first_neg([1]))
if [] or {} or "" or 0 or None:
    print("truthy?")
else:
    print("all falsy")
x = 5; y = 6; print(x + y)
if x: print("one-liner")
while False: pass
for _ in range(2): print("twice")
print("done")
