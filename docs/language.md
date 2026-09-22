# cpy language reference

cpy runs the Python language (a large, carefully-chosen subset — most everyday `.py` scripts run
unmodified). Source files use `.cpy` (or plain `.py`) and must be UTF-8.

## Lexical structure

- Blocks are made with indentation (spaces or tabs; a tab counts as 4 columns). Lines inside `()[]{}` continue freely; a trailing `\` also continues a line.
- Comments start with `#`.
- Numbers: `42`, `1_000`, `0x1F`, `0b101`, `0o17`, `3.14`, `1e-3`.
- Strings: `'a'`, `"a"`, `'''triple'''`, prefixes `r` (raw) and `f` (formatted); `b`/`u` are accepted and treated as plain strings. Escapes: `\n \t \r \0 \\ \' \" \xHH \uXXXX \UXXXXXXXX`. Adjacent literals are joined.
- Keywords: `and as assert break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield None True False`.

## Types

| type | notes |
|---|---|
| `int` | signed 64-bit; overflow raises `OverflowError` (see "Not supported") |
| `float` | IEEE double; printed with the shortest repr that round-trips |
| `bool` | `True`/`False`; behaves as 0/1 (`True + 1 == 2`) |
| `str` | immutable UTF-8; indexed and sliced by character |
| `list` / `tuple` | ordered; tuples are immutable and hashable |
| `dict` | insertion ordered; keys may be `None`, bool, int, float, str, tuple |
| `set` / `frozenset` | hash set; frozenset is immutable and hashable |
| `range` | lazy arithmetic sequence |
| generators, `map`/`filter`/`zip`/`enumerate`/`reversed`/`iter` results | lazy iterators, produced one item at a time |
| `None`, functions, classes, instances, modules, files | |

Truthiness: `None`, `0`, `0.0`, `""`, empty containers, and instances whose `__bool__`/`__len__` say so are false.

## Expressions

Operator precedence (low → high): `lambda`, `if/else`, `or`, `and`, `not`, comparisons (`< <= > >= == != in not in is is not`, chainable), `|`, `^`, `&`, `<< >>`, `+ -`, `* / // %`, unary `+ - ~`, `**` (right associative), call / subscript / attribute.

- `/` always gives a float; `//` and `%` follow Python's floor semantics.
- `+`/`*` work on `str`/`list`/`tuple`; `|` `&` `-` `^` work on `set`/`dict` (`dict | dict` merges); `%` on `str` formats.
- Slices `a[i:j:k]` work on `str`/`list`/`tuple`/`range`; lists support slice assignment and `del a[i:j]`.
- Unpacking: `[*a, *b]`, `(*a, x)`, `{*a, *b}`, `{**d1, **d2}`, and in calls `f(*args, **kwargs)`.
- The walrus operator: `if (n := len(x)) > 3: ...`.
- Comprehensions — list `[x*x for x in xs if p(x)]`, set `{x for x in xs}`, dict `{k: v for k, v in pairs}`, and **generator expressions** `(x for x in xs)`, which are lazy (evaluated on demand, not built into a list).
- f-strings: `f"{expr}"`, `f"{expr!r}"`, `f"{value:>8.2f}"`, `f"{x=}"` (debug form), `{{`/`}}` for braces. Format specs support fill/align (`< > ^ =`), sign, `0`, width, `,`, precision and types `d x X o b c e f g % s`.

## Statements

```python
x = y = 0                      # chained assignment
a, b = b, a + b                # tuple unpacking, incl. nested and starred: a, *rest = xs
x: int = 3                     # annotations are recorded in __annotations__ (unchecked) and executed
x += 1                         # + - * / // % ** & | ^ << >>
if c: ... elif d: ... else: ...
while cond: ... else: ...      # else runs unless the loop was left with break
for i in range(10): ... else: ...
break; continue; pass
del x; del lst[0]; del d["k"]; del lst[1:3]
assert cond, "message"
global name; nonlocal name
```

## Functions

```python
def f(a, b=2, *args, c, d=4, **kwargs):   # defaults, *args, keyword-only (c, d), **kwargs
    return a, b, args, c, d, kwargs

f(1, c=9)
g = lambda x, y=1: x + y

@decorator
def h(): ...
```

Type annotations on parameters and return types are parsed and ignored at runtime. Positional-only markers (`def f(a, b, /, c)`) are accepted.
Scoping follows Python: local → enclosing function (closure) → module globals → builtins; assigning creates a local unless declared `global`/`nonlocal`. The recursion limit is 1000 by default, adjustable with `sys.setrecursionlimit`.

### Generators

```python
def countdown(n):
    while n > 0:
        yield n
        n -= 1
    return "done"

for v in countdown(3):
    print(v)
g = countdown(2)
next(g); g.send(None); g.close(); g.throw(ValueError("x"))
```

`yield`, `yield from`, `.send()`, `.close()`, `.throw()`, and `return value` (available via `StopIteration.value` semantics through `yield from`) all work. Internally each generator runs on its own OS thread and is stepped like a coroutine, so it is fully lazy and can be infinite.

## Classes

```python
class Animal:
    sound = "..."
    def __init__(self, name):
        self.name = name
    def speak(self):
        return f"{self.name}: {self.sound}"

class Dog(Animal):
    sound = "woof"

class Labrador(Dog, Retriever):     # multiple inheritance, C3 MRO like Python
    pass
```

- `super()` (zero-arg, inside a method, or `super(Cls, obj)`), `isinstance`, `issubclass`, `type(x)`, `type(name, bases, dict)`.
- Special methods: the full common arithmetic/comparison/container set (`__init__ __repr__ __str__ __eq__ __lt__ ... __le__ __gt__ __ge__ __add__ ... __iadd__ ... __neg__ __len__ __bool__ __hash__ __getitem__ __setitem__ __contains__ __call__ __enter__ __exit__ __iter__ __next__ __getattr__ __setattr__ __new__ __init_subclass__ __class_getitem__`).
- `@staticmethod`, `@classmethod`, `@property` (with `.setter`/`.deleter`), and any user decorator.
- Class attributes, `__dict__`, `__mro__`, `__bases__`; every class implicitly derives from `object`.

## Exceptions

```python
try:
    risky()
except (ValueError, KeyError) as e:
    print("bad:", e)
except Exception:
    raise
else:
    print("no error")
finally:
    cleanup()
```

Hierarchy: `BaseException → Exception → ArithmeticError (ZeroDivisionError, OverflowError), LookupError (IndexError, KeyError), ValueError, TypeError, NameError (UnboundLocalError), AttributeError, RuntimeError (RecursionError, NotImplementedError), ImportError, SyntaxError, AssertionError, StopIteration, EOFError, OSError (FileNotFoundError, FileExistsError; alias IOError)`, plus `KeyboardInterrupt`, `GeneratorExit`, `SystemExit`. Define your own by subclassing `Exception`.

## Modules and packages

```python
import math
import pkg.sub               # dotted imports and packages (pkg/__init__.cpy, pkg/sub.cpy) work
from pkg import sub as s
from . import sibling         # relative imports, using __package__
from collections import Counter, deque
```

A `.cpy` or `.py` file is searched along `sys.path` (script directory, then the current directory, then `$CPY_PATH`); a directory with `__init__.cpy`/`__init__.py` is a package. `__name__`, `__file__`, `__package__` are set as in Python.

## Standard library

Built into the binary (no filesystem needed): `math`, `random`, `time`, `sys`, `os`, `os.path`,
`collections` (`deque`, `Counter`, `defaultdict`, `OrderedDict`, `namedtuple`, `ChainMap`),
`itertools` (`chain`, `count`, `cycle`, `repeat`, `islice`, `product`, `permutations`, `combinations`,
`groupby`, `accumulate`, `zip_longest`, `takewhile`, `dropwhile`, `starmap`, `compress`, `pairwise`, ...),
`functools` (`reduce`, `partial`, `lru_cache`/`cache`, `wraps`, `cmp_to_key`, `total_ordering`),
`operator`, `heapq`, `bisect`, `string`. See [docs/builtins.md](docs/builtins.md).

## `with`

```python
with open(path) as f, open(other) as g:
    ...
```

Multiple context managers and any object with `__enter__`/`__exit__` are supported (`__exit__` returning a true value swallows the exception).

## Not supported

Bytes/bytearray, complex numbers, arbitrary-precision integers (int is 64-bit; overflow raises `OverflowError`), `match`/`async`/`await`, `__slots__`, `__del__`, metaclasses beyond the 3-argument `type(...)` form, `str.encode`, multi-line `with (a, b):` parenthesised form, exception groups / `except*`.

Case conversion (`upper`, `lower`, `title`, …) and `isalpha` know Latin (including Vietnamese), Greek and Cyrillic letters; other scripts are left unchanged.
