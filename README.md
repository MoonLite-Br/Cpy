# cpy

**Python syntax, C speed.** cpy runs real Python programs — classes, generators, exceptions,
sets, decorators, `collections`/`itertools`/`functools` — through a from-scratch interpreter
written in portable C, with no VM bytecode, no GC pauses, and (on the benchmarks in `tests/`)
noticeably less overhead than CPython on loops, string building, list/dict work and attribute
access. One binary, no dependencies, built to be developed and run on a phone (Termux) as well
as on a PC or server.

```python
from collections import Counter

class Greeter:
    def __init__(self, name):
        self.name = name
    def greet(self):
        return f"Hello, {self.name}!"

for who in ["world", "cpy"]:
    print(Greeter(who).greet())

def fib():
    a, b = 0, 1
    while True:
        yield a
        a, b = b, a + b

print([n for _, n in zip(range(10), fib())])
print(Counter("mississippi").most_common(2))
```

## Install

```bash
# Termux:  pkg install clang make
./install.sh          # builds and installs to $PREFIX/bin (Termux), /usr/local/bin or ~/bin
cpy                    # REPL
cpy file.py            # run a script (.cpy or .py both work)
cpy -c 'print(1 + 2)'
```

Only `make` + a C compiler are needed (`./build.sh` just builds `bin/cpy`).

## What you get

- **Full-looking syntax**: classes with multiple inheritance and a real C3 MRO, `@decorator`, `@property`/`@staticmethod`/`@classmethod`, `*args`/`**kwargs`/keyword-only params, generators (`yield`, `yield from`, `.send`/`.throw`/`.close`), generator/list/set/dict comprehensions, f-strings (incl. `f"{x=}"`), the walrus operator, star-unpacking, `with` (multiple managers), `try/except/else/finally`, packages and relative imports.
- **Types that behave like Python's**: `int` (64-bit)/`float`/`str` (UTF-8)/`bool`/`list`/`tuple`/`dict`/`set`/`frozenset`/`range`, all sharing references the way Python objects do.
- **Batteries**: `math`, `random`, `time`, `sys`, `os`(`.path`), `collections`, `itertools`, `functools`, `operator`, `heapq`, `bisect`, `string` — built into the binary, no install step.
- **Built for speed**: variables are resolved to array slots at parse time (no dict lookup for locals), hot paths for integer arithmetic/comparisons/calls skip the generic dispatcher, small call frames are recycled instead of malloc'd. See [docs/internals.md](docs/internals.md#performance-design).
- **Helpful tooling**: tracebacks with file/line/function, a REPL that keeps state across multi-line blocks.

Everything the language can do is documented in [docs/language.md](docs/language.md) and [docs/builtins.md](docs/builtins.md).

## How fast

Rough, single-machine numbers (`python3` is CPython 3.12; see `tests/` for the exact scripts) — a numeric loop, building a list, filling and reading a dict, and a hot attribute-access loop all come out **1.2×–50× faster** than CPython; a call-heavy recursive benchmark (`fib(30)`) is about on par. cpy is not a JIT and won't out-run PyPy, but for scripts, small tools and glue code it comfortably beats the stock interpreter while staying source-compatible.

## Differences from Python (short list)

`int` is 64-bit (arithmetic overflow raises `OverflowError` instead of growing to bigint); no bytes/bytearray, complex numbers, `match`, `async`/`await`, `__slots__`, or metaclasses beyond the 3-argument `type(...)` form. The full list is in [docs/language.md](docs/language.md#not-supported). cpy also keeps two small conveniences from its early versions: `read("file")` and `write("file", text)`.

## Project layout

```
src/            the interpreter (lexer, parser, resolver, evaluator, generators, builtins)
lib/*.cpy       standard-library modules written in cpy, baked into the binary at build time
tests/          test-suite (tNN_name.cpy + expected tNN_name.out), verified against CPython 3.12
examples/       small example programs
docs/           language reference, builtin reference, internals
run_tests.sh    run all tests (make test)
.github/        CI: Linux, Linux arm64, Android/Termux arm64 (NDK) builds
```

## Tests

```bash
make test                 # or ./run_tests.sh
make debug && CPY=./bin/cpy-asan ASAN_OPTIONS=detect_leaks=0 ./run_tests.sh   # with sanitizers
tests/compare_python.sh   # (developer) diff cpy against CPython on the shared-syntax tests
```
