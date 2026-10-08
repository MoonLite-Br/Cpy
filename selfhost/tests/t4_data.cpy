lst = [1, 2, 3]
lst[0] = 99
print(lst, len(lst), sum(lst))

d = {"a": 1, "b": 2}
d["c"] = 3
print(d["a"], d["c"], len(d))

for i in range(5):
    print(i * i)

words = ["pear", "apple", "kiwi"]
print(sorted(words))
for i, w in enumerate(words):
    print(i, w)
