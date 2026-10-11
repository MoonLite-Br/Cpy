total = 0
i = 0
while i < 10:
    if i % 2 == 0:
        total = total + i
    i = i + 1
print(total)

for x in [1, 2, 3, 4, 5]:
    if x == 3:
        continue
    if x == 5:
        break
    print(x)

n = 7
if n < 5:
    print("small")
elif n < 10:
    print("medium")
else:
    print("large")
