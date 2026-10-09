# cpy-in-cpy — a self-hosted interpreter

This directory is a second, independent cpy interpreter — lexer, parser and
tree-walking evaluator — written **entirely in cpy** and executed by the real
(C) `cpy` binary. It doesn't share a single line with `src/`.

```bash
cpy selfhost/cpy.cpi yourscript.cpi
```

## Why this exists, and why it is not the fast path

Self-hosting proves the language is expressive enough to describe itself
(closures, classes, exceptions-as-control-flow, dict/list literals — all used
below). It is **not** a way to make cpy faster — the opposite: every
statement your script runs is now walked twice, once by this cpy code and
once more by the C interpreter running *that* code. The benchmark below is
representative:

| running `fib(22)` | time |
|---|---|
| native `bin/cpy` (C) | ~0.005 s |
| CPython 3.12 | ~0.013 s |
| `cpy selfhost/cpy.cpi` (cpy-in-cpy) | ~0.76 s |

So: use `bin/cpy` directly for anything performance-sensitive — that's the
whole point of the C interpreter in `src/`. Reach for this self-hosted one
when you specifically want to **run untrusted or dynamically-generated code
inside a cpy program** without shelling out, since it has none of the host's
filesystem/process access unless you hand it a builtin that provides it, or
as a compact reference implementation to study or extend (e.g. as a starting
point for a future bytecode compiler — see "Ideas" below).

## Scope of the subset

Supported: numbers, strings, `True`/`False`/`None`, `list`/`dict` literals
and indexing, basic slicing (`a[lo:hi]`, no step), `+ - * / // % **`
comparisons, `and`/`or`/`not`, `if/elif/else`, `while`, `for x in ...` (with
tuple-target `for k, v in ...`), `break`/`continue`, `def` with positional
defaults and closures, `return`, `class` with single inheritance and
`self`/`__init__`, `global`.

Not implemented (raises a clear parse/runtime error rather than doing the
wrong thing silently): `try/except`, `*args`/`**kwargs`, keyword arguments at
call sites, f-strings, comprehensions, `with`, `import`, decorators,
augmented slice/attribute targets, multiple inheritance. Adding any of these
means touching `lexer.cpi`/`parser.cpi`/`interp.cpi` — each is a few hundred
lines and follows the same structure as the C sources in `../src/` for the
same feature, so `../src/parser.c` / `../src/interp.c` are the reference to
copy the logic from.

## Layout

```
lexer.cpi    text -> Token list (handles INDENT/DEDENT, strings, numbers)
parser.cpi   Token list -> AST (plain dicts, key "kind")
interp.cpi   AST -> result (Env is a dict + parent pointer; classes/instances
             are plain objects; break/continue/return are implemented by
             raising and catching cpy exceptions, same trick the C
             interpreter uses internally with setjmp/longjmp)
cpy.cpi      entry point: reads argv[1], runs it
tests/       t*.cpi scripts whose output is checked against the real
             interpreter by ../selfhost_test.sh
```

## AOT: compiling cpy to a real native binary

Alongside the tree-walking `cpy.cpi` above, this directory also has a genuine
**ahead-of-time compiler**: `codegen.cpi` walks the same AST and emits C
source that calls straight into the runtime (`binop()`, `val_cmpop()`,
`call_value()`, ...); `aotc.cpi` then shells out to `cc` to turn that C into
a real machine-code executable, linked against `bin/libcpyrt.a` (built by
`make aot`, and by `install.sh`).

```bash
make aot                                            # build bin/libcpyrt.a once
cpy selfhost/aotc.cpi fib.cpi -o fib                # compile
./fib                                                # run — real machine code, no AST left
```

This is the "self-hosting *and* fast" direction: unlike the tree-walker, a
compiled call is a real C call (no `Env`, no per-node dispatch), and a
`for i in range(...)` loop becomes a real C `for` loop. Measured on this
machine, AOT-compiled code ran **2–4.5× faster than the interpreter and
2.4× faster than CPython 3.12** on both a recursive (`fib`) and a
loop-heavy benchmark.

The trade-off is scope: v1 only compiles **numeric-only** cpy — `int`,
`float`, `bool`, `None`, arithmetic, comparisons, `if/while/for x in
range(...)`, and plain functions/recursion. No strings, lists, dicts, or
classes yet. This isn't an arbitrary restriction: numeric `Value`s aren't
heap-allocated in the runtime, so the generated C never has to reproduce the
interpreter's reference-counting discipline, which keeps this compiler both
small and safe (checked under ASan). Anything outside the subset is
rejected with a clear `CompileError` (source line included) at compile
time rather than doing the wrong thing silently — e.g. `aotc fib.cpi`
reports exactly which line and construct to fix, or that the script just
needs the regular interpreter instead.

`import` works for real modules (`math`, `random`, ...): `import math` and
calls like `math.sqrt(x)` or reads like `math.pi` compile into the same
generic runtime calls (`import_module`, `get_attr`, `call_value`) the
tree-walking interpreter itself would make — so a compiled loop around a
`math.sqrt` call is still fully native, only the call itself pays the usual
dynamic-dispatch cost. `print`, `abs`, `round`, `min`, `max`, `pow`, and
`divmod` also work directly, in expressions or as statements (resolved once
at program start, not re-looked-up every call). `import` is AOT-only for
now — the tree-walking `cpy.cpi` above doesn't support it yet (running an
`import`-using script through `cpy selfhost/cpy.cpi` gives a clear `cannot
execute node kind 'import'` error rather than silently skipping it).

Growing the subset to strings/lists/dicts is the natural next step, and
means teaching `codegen.cpi` the same incref/decref discipline `src/interp.c`
already follows (see the comment at the top of `src/value.c`) -- doable, just
more bookkeeping per node.

## Ideas for next steps

- A `try/except` in `interp.cpi` is the most valuable gap to close next since
  the C interpreter's own approach (catch a Python exception per AST
  try-block) translates directly.
- Compiling the AST to a flat bytecode list (instead of walking dicts) before
  evaluating would close most of the 150x gap to the native interpreter,
  while still being "self-hosted" — that's the natural next milestone if you
  want a faster self-hosted tier rather than just a demo.
