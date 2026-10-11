l = [3, 1, 2]
l.append(5)
l.extend([7, 8])
l.insert(0, 99)
print(l, len(l), l[0], l[-1], l[1:3], l[::-1], l.index(5), l.count(1))
print(l.pop(), l.pop(0), l)
l.remove(5)
l.sort()
print(l)
l.sort(reverse=True)
print(l)
l.reverse()
print(l, sorted([3, 1, 2]), sorted("bca"), sorted([3, 1, 2], reverse=True), sorted(["bb", "a", "ccc"], key=len))
print([1, 2] + [3], [0] * 3, [1, 2, 3] == [1, 2, 3], [1, 2] < [1, 3], 2 in [1, 2], [] == [], [[1], [2]])
m = [[0] * 2 for _ in range(3)]
m[1][1] = 5
print(m)
a = [1, 2, 3]
b = a
b.append(4)
c = a.copy()
c.append(5)
print(a, b, c, a is b, a is c)
a += [9]
a *= 2
print(a)
del a[0]
del a[-1]
print(a)
t = (1, 2, 3)
print(t, t[1], len(t), t + (4,), t * 2, (1,), (), t[::-1], t.index(2), t.count(2), t == (1, 2, 3))
x, y, z = t
print(x, y, z)
(p, q), r = (1, 2), 3
print(p, q, r)
d = {"a": 1, "b": 2}
d["c"] = 3
d["a"] += 10
print(d, len(d), d["a"], d.get("z"), d.get("z", 0), "a" in d, "z" in d, list(d), list(d.keys()), list(d.values()), list(d.items()))
print(d.pop("a"), d.pop("zz", "none"), d.setdefault("k", 5), d.setdefault("b", 100), d)
d.update({"b": 20, "n": 1})
d.update(x=1)
print(d)
del d["n"]
for k, v in d.items():
    print(k, v, end="; ")
print()
e = dict(a=1, b=2)
f = dict([("x", 1), ("y", 2)])
g = dict(e)
g["a"] = 100
print(e, f, g, {}, {1: "a", 2.5: "b", (1, 2): "c", None: "d", True: "e"})
print(len(d.copy()), {k: v * 2 for k, v in e.items()}, {x: x * x for x in range(4)})
sq = [x * x for x in range(6)]
print(sq, [x for x in sq if x % 2 == 0], [(x, y) for x in range(2) for y in range(2)], [c.upper() for c in "abc"])
print(list(range(5)), list(range(2, 10, 3)), list(range(5, 0, -2)), len(range(10)), range(5)[2], 3 in range(5), 7 in range(0, 10, 2))
print(list(zip([1, 2, 3], "ab")), list(enumerate(["a", "b"], start=1)), list(map(lambda v: v + 1, [1, 2])), list(filter(lambda v: v > 1, [1, 2, 3])), list(reversed([1, 2, 3])))
print(any([0, 0, 1]), all([1, 1, 0]), any([]), all([]), min("bca"), max([1, 5, 3], key=lambda v: -v), sum(x for x in range(5)), sum([[1], [2]], []))
print(list("abc"), tuple([1, 2]), list((1, 2)), list({1: 2}), dict(zip("ab", [1, 2])))
words = "the quick brown fox jumps over the lazy dog the end".split()
counts = {}
for w in words:
    counts[w] = counts.get(w, 0) + 1
print(sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:3])
nested = {"a": [1, 2, {"b": (3, 4)}]}
print(nested, nested["a"][2]["b"][1])
big = list(range(1000))
print(sum(big), len(big[::7]), big[-3:], big[10:13])
