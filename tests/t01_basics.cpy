# arithmetic, precedence, numbers
print(1 + 2 * 3, (1 + 2) * 3, 7 // 2, -7 // 2, 7 % 3, -7 % 3, 7 % -3, 2 ** 10, 2 ** -1, 7 / 2)
print(10 / 4, 10 // 4, 10.0 // 4, -10.5 % 3, 1e3, 1.5e-7, 123456789.0, 1e16, 0.1 + 0.2)
print(0x1F, 0b101, 0o17, 1_000_000, 5 & 3, 5 | 3, 5 ^ 3, 1 << 4, 256 >> 2, ~5)
print(True + True, True and False, not True, 3 > 2 > 1, 1 < 2 < 2, 1 == 1.0)
print(abs(-5), abs(-2.5), min(3, 1, 2), max([4, 9, 2]), sum([1, 2, 3]), sum([0.5, 0.25]))
print(round(2.5), round(3.5), round(-0.5), round(3.14159, 2), divmod(17, 5), divmod(-17, 5), pow(2, 10), pow(3, 4, 5))
print(int("42"), int(" 7 "), int(3.9), int(-3.9), float("1.5"), float(3), str(12), str(1.0), bool(0), bool("a"))
print(int("ff", 16), hex(255), bin(5), oct(8), chr(65), ord("A"), chr(233), ord("é"))
x = 10
x += 5
x -= 3
x *= 2
x //= 5
x **= 2
x %= 7
print(x)
y = 7
y /= 2
print(y)
a = b = 5
print(a, b)
a, b = b + 1, a
print(a, b)
a, b = b, a
print(a, b)
print(None, True, False, None is None, 1 is not None)
print(1 if True else 2, "yes" if 0 else "no")
print(5 and 6, 0 and 6, 0 or 7, "" or "x", None or [])
print(type(1) == int, type("a") == str, isinstance(1, int), isinstance(True, int), isinstance("a", (int, str)), isinstance(1.5, float))
