def add(a, b): return a + b
def sub(a, b): return a - b
def mul(a, b): return a * b
def truediv(a, b): return a / b
def floordiv(a, b): return a // b
def mod(a, b): return a % b
def pow(a, b): return a ** b
def neg(a): return -a
def pos(a): return +a
def abs(a): return a if a >= 0 else -a
def eq(a, b): return a == b
def ne(a, b): return a != b
def lt(a, b): return a < b
def le(a, b): return a <= b
def gt(a, b): return a > b
def ge(a, b): return a >= b
def not_(a): return not a
def truth(a): return bool(a)
def is_(a, b): return a is b
def is_not(a, b): return a is not b
def and_(a, b): return a & b
def or_(a, b): return a | b
def xor(a, b): return a ^ b
def contains(a, b): return b in a
def concat(a, b): return a + b
def getitem(a, b): return a[b]
def index(a): return int(a)

class itemgetter:
    def __init__(self, *items):
        self._items = items
    def __call__(self, obj):
        if len(self._items) == 1:
            return obj[self._items[0]]
        return tuple(obj[i] for i in self._items)

class attrgetter:
    def __init__(self, *attrs):
        self._attrs = attrs
    def _get(self, obj, name):
        for part in name.split("."):
            obj = getattr(obj, part)
        return obj
    def __call__(self, obj):
        if len(self._attrs) == 1:
            return self._get(obj, self._attrs[0])
        return tuple(self._get(obj, a) for a in self._attrs)

class methodcaller:
    def __init__(self, name, *args, **kwargs):
        self._name = name
        self._args = args
        self._kwargs = kwargs
    def __call__(self, obj):
        return getattr(obj, self._name)(*self._args, **self._kwargs)
