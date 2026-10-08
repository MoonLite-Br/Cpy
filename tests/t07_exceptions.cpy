def risky(n):
    if n == 0:
        raise ValueError("zero!")
    if n == 1:
        raise KeyError("k")
    if n == 2:
        return 1 / 0
    if n == 3:
        return [1][5]
    if n == 4:
        return {}["missing"]
    if n == 5:
        return int("abc")
    if n == 6:
        return undefined_name
    if n == 7:
        return "a" + 1
    if n == 8:
        return None.foo
    return "ok"
for i in range(10):
    try:
        print(i, risky(i))
    except ValueError as e:
        print(i, "ValueError:", e)
    except (KeyError, IndexError) as e:
        print(i, "lookup:", type(e).__name__, e)
    except ZeroDivisionError as e:
        print(i, "zero:", e)
    except Exception as e:
        print(i, "other:", type(e).__name__)
try:
    try:
        raise RuntimeError("inner")
    finally:
        print("cleanup 1")
except RuntimeError as e:
    print("caught", e)
def f():
    try:
        return "try"
    finally:
        print("finally runs")
print(f())
def g():
    for i in range(3):
        try:
            if i == 1:
                continue
            print("body", i)
        finally:
            print("fin", i)
g()
try:
    x = 1
except Exception:
    print("no")
else:
    print("else ran", x)
class MyErr(Exception):
    def __init__(self, code):
        super().__init__("code " + str(code))
        self.code = code
try:
    raise MyErr(42)
except MyErr as e:
    print(e, e.code, isinstance(e, Exception))
try:
    try:
        raise ValueError("a")
    except ValueError:
        raise
except ValueError as e:
    print("reraised", e)
try:
    raise TypeError
except TypeError as e:
    print("bare class ->", repr(str(e)))
try:
    assert 1 == 2, "math broke"
except AssertionError as e:
    print("assert:", e)
def deep(n):
    return deep(n + 1)
try:
    deep(0)
except RecursionError:
    print("recursion caught")
try:
    a, b = [1, 2, 3]
except ValueError as e:
    print("unpack:", e)
try:
    [].pop()
except IndexError as e:
    print(e)
try:
    int(None)
except TypeError:
    print("type error")
try:
    raise Exception("plain")
except:
    print("bare except")
print("end")
