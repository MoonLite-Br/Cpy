# Internals

## Pipeline

```
source ──lexer.c──▶ tokens ──parser.c──▶ AST ──resolve.c──▶ AST + scope info ──interp.c──▶ result
```

| file | role |
|---|---|
| `src/cpy.h` | all shared types and prototypes |
| `src/lexer.c` | tokenizer: indentation, strings/f-strings, numbers, operators |
| `src/parser.c` | recursive-descent parser → `Node` tree |
| `src/resolve.c` | static scope analysis: turns every name into a slot index, a closure hop, or a global lookup |
| `src/interp.c` | evaluator, classes (C3 MRO), exceptions, imports/packages, the tiny prelude |
| `src/value.c` | values, strings, lists, the shared hash table (backs dict/set), equality/ordering, repr, formatting |
| `src/gen.c` | iterators (`map`/`filter`/`zip`/... as lazy objects) and generators (one OS thread each, stepped like coroutines) |
| `src/builtins.c` | global builtin functions |
| `src/methods.c` | str/list/dict/set/file methods, native `math`/`random`/`_time`/`sys`/`os`/`_itertools` |
| `lib/*.cpy` | the parts of the standard library written in cpy itself (`collections`, `itertools`, `functools`, `operator`, `heapq`, `bisect`, `string`, `time`); baked into the binary at build time by `tools/gen_stdlib.sh` → `src/stdlib_data.c` |
| `src/main.c` | command line, REPL; runs on a big-stack thread |

## Performance design

- **Values** are a 16-byte tagged union — no boxing for `int`/`float`/`bool`/`None`.
- **Static scope resolution** (`resolve.c`) means a local variable access is an array index into the call frame, not a dict/string lookup; free variables are a fixed number of parent hops; only true module globals go through a (cached-index) dict lookup. This is the single biggest difference from a naive interpreter and from CPython's LOAD_FAST-via-dict fallback path.
- **Small-object recycling**: call frames (`Env`) with ≤8 slots come from a free list instead of malloc/free.
- **Hot/cold split** in `interp.c`: `eval`/`exec_stmt` handle the common node kinds (name, constant, binop, compare, call, if/return/expr-statement) inline with an integer fast path (`int op int` skips the generic dispatcher entirely); everything else falls through to `eval_cold`/`exec_cold`.
- **Direct call path**: `x.method(args)` and `f(args)` with no `*`/`**`/keywords go through `eval_call`, which resolves attribute-call binding without allocating an intermediate bound-method object, and calls `call_func` directly (no dispatch through `call_value`).
- Reference counting frees temporaries immediately — long loops run in constant memory; reference cycles are not collected.

## Scopes

`resolve.c` walks the AST once after parsing and annotates every `N_NAME` node with how to reach it (`RK_LOCAL`/`RK_FREE`/`RK_GLOBAL`/`RK_CLASS`/`RK_DYN`), and every function/lambda/comprehension with its flat list of local slots. `interp.c` never touches a dict for a local or free variable — only module-level globals and class bodies use one (the same dict is also the class's `__dict__`/module namespace).

## Generators

Each generator gets its own OS thread with an 8&nbsp;MB stack, started lazily on first `next()`/`send()`, and two semaphores hand control back and forth — the generator thread and the caller are never both running at once, so there's no real concurrency, just a convenient way to keep a full interpreter C call stack "parked" at a `yield`. Closing a generator (garbage collection or `.close()`) throws `GeneratorExit` into it so `finally` blocks run, then joins the thread.

## Exceptions

Exceptions are instances of classes defined by a small cpy prelude (see `PRELUDE` in `interp.c`). `raise` and runtime errors use `setjmp`/`longjmp` to unwind to the nearest `try` (a `Handler` chain); tracebacks come from the `Frame` chain, captured when the exception is thrown.

## Imports

`import_module` resolves dotted names component by component, checks the native module table, then searches `sys.path` for `name.cpy`/`name.py`/`name/__init__.cpy`, then falls back to the embedded `lib/*.cpy` sources compiled into the binary. A module that fails while importing is removed from the module cache so a later `import` can retry.

## Adding things

A new builtin function: `reg_builtin("name", fn)` in `builtins_init()` (`builtins.c`). A new method on a builtin type: `regm(table, "name", fn)` in `methods_init()` (`methods.c`). A new native module: add a branch in `builtin_module()` (`methods.c`). A pure-cpy stdlib module: drop a `.cpy` file in `lib/`, it's picked up by `Makefile`/`tools/gen_stdlib.sh` automatically. Raise errors with `throw_error("ValueError", "format %d", x)`. Add a test to `tests/` and run `./run_tests.sh --update` (review the diff).

## Limits

- Recursion limit 1000 by default (`sys.setrecursionlimit`); the interpreter runs on a thread with a 512&nbsp;MB (falling back to 128/32&nbsp;MB) stack.
- Integers are `int64_t` with overflow checks (`OverflowError`).
- Strings store UTF-8 plus a cached character count; indexing non-ASCII strings is O(n).
