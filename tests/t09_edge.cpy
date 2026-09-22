# edge cases
print(2 ** 62, -2 ** 62, 9223372036854775807, -9223372036854775807 - 1)
print(1e308 * 10, -1e308 * 10, 1 / 3, 2 / 3, 10 ** 15 / 7, 5e-324, 1.7976931348623157e308)
print(float("inf"), float("-inf"), float("nan") == float("nan"), 0.1 * 3, 1e22, 1e21, 123456789012345678.0, 1.5e300)
print(3.0, 3.10, -0.5, 100.0, 1234567.891, 0.000123, 0.0001, 0.00001)
print(7 // 2.0, -7 // 2.0, 7 % 2.5, -7 % 2.5, 2 ** 0.5, (-8) ** 2, -8 ** 2, 2 ** 3 ** 2, (2 ** 3) ** 2)
print(1 < 2 == 2, 1 == 1 != 2, "a" < "b" < "c", 3 in [1, 2, 3] in [[1, 2, 3]])
print(bool([]), bool([0]), bool(""), bool(" "), bool(0.0), bool(None), bool({}), bool(()))
print("a\tb\\n", 'q"q', "q'q", "multi" "part", len("\n"), "\x41\102" if False else "\x41")
print([] is [], () == (), {} == {}, [1] == [1], "a" is "a" if False else True)
lst = [(2, "b"), (1, "x"), (2, "a"), (1, "y")]
print(sorted(lst), sorted(lst, key=lambda p: p[0]), sorted(lst, key=lambda p: p[0], reverse=True))
words = ["banana", "Apple", "cherry", "apple"]
print(sorted(words), sorted(words, key=str.lower) if False else sorted(words, key=lambda w: w.lower()))
d = {}
for i in range(100):
    d[i] = i * i
for i in range(0, 100, 3):
    del d[i]
for i in range(100, 130):
    d[i] = -i
print(len(d), sum(d.values()), list(d)[:5], list(d)[-3:], 5 in d, 3 in d)
d2 = {"b": 1, "a": 2}
d2["c"] = 3
del d2["b"]
d2["b"] = 9
print(d2, list(d2.items()))
x = [1, 2, 3]
x[1:] = x[1:]
x[1:2] = [7, 8, 9]
x[:1] = []
x[len(x):] = ["end"]
del x[0]
del x[::2]
x = [1, 2, 3]
y = x[:]
y[0] = 100
print(x, y, x[5:], x[-10:2], x[::-1][0], x[:0], x[10:20])
s = "abcdef"
print(s[2:], s[:2], s[-2:], s[1:-1], s[::-1], s[::3], s[5:1:-1], s[-1:-4:-1], s[100:], s[:100])
n = None
print(n is None, n == None, n != 0, [n], str(n), n if n else "empty")
print(int(True), float(False), True == 1, True + 1, sum([True, True, False]), max(True, 0), "x" * 0, "x" * -1)
print(divmod(7, -2), divmod(-7, -2), divmod(7.5, 2), round(1234.5678, -2), round(-1.5), round(0.5), round(1.5), round(2.675, 2))
print(list(range(-3, 3)), list(range(10, 0, -3)), list(range(0)), list(range(3, 3)), range(0, 10, 3)[-1])
print("%s|%5s|%-5s|%05d|%+d|%.3f|%e|%g|%x|%o" % ("a", "b", "c", 42, 42, 3.14159, 12345.678, 0.0001, 255, 8))
print("%d%%" % 50, "%s" % None, "%s" % [1, 2], "%s %s" % ((1, 2), 3), "%r %r" % ("s", 1))
print(f"{'nested ' + f'{1 + 1}'}", f"{3 if True else 4}", f"{[i for i in range(3)]}", f"{ {'a': 1}['a'] }", f"{1:>3}{2:<3}|")
print(str(1.0), str(10.0 ** 20 / 1), repr(1.5), repr("a'b\"c"), repr(None), repr([None, True, "s", 1.0, (1, 2), {"k": [1]}]))
print(abs(-0.0), -0.0, 0.0 == -0.0, 5 // 1, -5 // 1, 5.5 // 1, 1e3 // 7, 10 % 3.5)
print(max(1, 2.5), min([3, 1.5]), max("a", "b"), max([[1, 2], [1, 3]]), min((1, 2), (1, 1)), max({"a": 1, "b": 2}))
print(len({}), len([]), len(""), len(range(0)), len({1: 1}), len((1, 2, 3)))
print("é".upper(), "ÀÉÎ".lower(), "đường".upper(), "ĐƯỜNG".lower(), "ǅ" if False else "x", "ß" if False else "y", "ư".upper(), "Ơ".lower())
print("hello world foo".title(), "hELLO".capitalize(), "ÉCOLE".capitalize(), "àbc".isalpha(), "ĂN".isupper(), "ăn".islower())
i = 0
while True:
    i += 1
    if i > 5:
        break
else:
    pass
print(i)
print(((((1)))), (1,), (1, 2)[1], [1, 2, 3][-1], {"a": {"b": {"c": 1}}}["a"]["b"]["c"])
a = [1, 2, 3]
for v in a:
    if v == 1:
        a.append(4)
print(a)
def gen_list(n):
    out = []
    for i in range(n):
        out.append(lambda i=i: i * i)
    return [f() for f in out]
print(gen_list(4))
print((lambda *args: sum(args))(1, 2, 3), (lambda x, y=2: x * y)(4))
big = "x" * 100000
print(len(big), big[-1], len(big * 3), big.count("x"))
print("end")
