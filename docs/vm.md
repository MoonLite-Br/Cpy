# Bytecode VM (src/vm.c)

An optional execution tier that sits *under* the tree-walker (nothing was removed).

* Each function body is compiled once, on its first call, into register 3-address code and run
  by `vm_run()`. Registers are the function's own `Env` slots (locals first, then compiler
  temporaries), so closures, tracebacks, `nonlocal`, `del` and exceptions behave as before.
* Natively compiled: locals/consts, arithmetic, comparisons (fused compare+jump), `and/or/not`,
  conditional expressions, `if/while/for` (incl. `else`, `break`, `continue`), `x += y`,
  calls (plain and method), indexing, attribute loads, `return`.
* Everything else (`try`, `with`, `def`, `class`, `import`, comprehensions, kwargs/star calls, ...)
  is delegated to the tree-walker via `VM_EVAL` / `VM_EXEC`, so every function compiles.
* Generators, coroutines and lambdas stay on the tree-walker. Module-level code too (for now).
* Switch off: `--no-vm` or `CPY_NOVM=1`. Inspect bytecode: `CPY_VMDUMP=1`.
* Safety net: `run_tests.sh` runs every test in both modes and diffs them;
  `tests/vm_fuzz.py N SEED` generates random programs and diffs both modes.

Typical speedup over the tree-walker: 1.15x (call/OOP-heavy) to ~3x (numeric loops).

## Generators on Android/Termux
Bionic has no `ucontext`, so generators used to fall back to one OS thread per generator
(~8s for 100k yields on a phone). `src/gen.c` now has three engines, picked by the Makefile:
`ucontext` (glibc) > `sigaltstack` bootstrap + `_setjmp/_longjmp` (Bionic, pure POSIX, no
per-arch assembly) > threads. `make` prints which one was chosen; test the Termux path on
Linux with `make FORCE_SIGALT=1`.

## AOT (selfhost/aotc.cpy)
- **Traceback**: AOT-compiled binaries now push a real `Frame` per boxed
  function call (cleanup-attribute based, so every `return` path pops it
  automatically) and wrap `main()` in a `Handler`, so an uncaught exception
  gets a proper `Traceback (most recent call last): File ..., line N, in
  func` instead of just a bare class+message. The unboxed/`__fast` tier
  deliberately has NO frame tracking -- measured cost on fib(35) was +25%
  for the Frame writes alone, +75% once the cleanup-attribute pop needed to
  do it correctly was added, which isn't an acceptable trade for a tier
  that exists specifically to approach raw-C speed. An exception raised
  inside a chain of fast calls still gets the right class/message and the
  outermost boxed/module frame; it just won't show the fast functions
  themselves in between.
- **Fast functions can now call boxed functions/builtins as a bare
  statement** (result discarded) -- e.g. `print(n)`, `print(abs(x))`,
  calling a sibling that itself doesn't qualify for the fast tier -- and
  still get the fast/unboxed tier for the rest of their body. Previously
  this disqualified the whole function (and, for builtins specifically,
  crashed the compiler with an internal error -- a real bug, not just a
  missing feature). Only statement position qualifies: if the result is
  captured (even into a variable that's never read again) or used in an
  expression, the function stays on the boxed tier, since there's no
  general way to unbox an arbitrary builtin's return value back into the
  fast domain. See TypeInfer.stmt_call_boxable/arg_representable and
  Gen.gen_boxed_call_stmt_fast/gen_fast_arg_boxed in codegen.cpy.
- **`**` is now supported in the fast tier** when its result type is
  provable at compile time: float-involved (`x ** y` with either operand
  float) is always float regardless of the exponent's sign, so that's
  unconditional; `int ** int` is only representable when the exponent is a
  literal non-negative integer constant (`x ** 2`, `x ** 3`, ...) -- a
  non-constant or possibly-negative integer exponent keeps the whole
  expression on the boxed tier, because binop()'s own OP_POW picks int vs
  float based on the exponent's RUNTIME sign, which the fast tier's fixed
  per-expression C return type can't represent. Overflow checking
  (cpy_pow_i) matches binop() exactly.

## str(int) / int formatting
`str(n)` for an int, and int formatting generally (f-strings, print, repr,
join-of-str(int)s, ...) used to go through `vsnprintf("%lld", ...)` into a
growable Buf, then copy AGAIN into the final Str -- three passes (plus
snprintf's own format-string/locale overhead) for what's always just an
optional '-' and some ASCII digits. Replaced with a dedicated digit
formatter (`buf_add_int` for the general/embedded case, `int_to_str` for
`str(int)` specifically, skipping the Buf entirely). ~2x faster on
`str(i)` in a loop, bringing it to roughly CPython's speed; benefits
anything that formats ints into strings, not just that one call site.

## Self-hosted parser (selfhost/parser.cpy, used by aotc.cpy)
Was a much narrower subset of the language than the real C parser
(src/parser.c) -- anything it couldn't parse would fail the WHOLE file with
a raw "expected NEWLINE, got ..." error, even if the unparseable construct
had nothing to do with the numeric function someone actually wanted
AOT-compiled (a `try:` block guarding unrelated setup code, for instance).
Closed this gap: the self-hosted lexer+parser now handle try/except/
finally, with, raise, assert, del, nonlocal, from-import, lambda, is/is not/
not in, async def/await, yield/yield from, f-strings, and keyword
arguments at call sites -- the same constructs the real parser accepts.
Codegen still correctly rejects actually *compiling* any of these to AOT
(numeric-only, same as before) -- the fix is that parsing past them no
longer blocks the rest of the file. Every "call" node consumer in
codegen.cpy (5 call sites) and the self-hosted tree-walker (interp.cpy,
used only by selfhost_test.sh) was audited to either handle the new
`kwargs` field or explicitly reject it with a clear error -- several of
them would otherwise have silently DROPPED keyword arguments instead of
erroring (a real, if narrow, correctness bug, not just a missing feature).
New regression coverage: tests/parser/ (parses, differential-tested
against CPython) + parser_test.sh, wired into run_tests.sh.

Known remaining gap in this area: a top-level `class` (or anything else
AOT can't handle) still blocks the whole file even if nothing numeric
actually uses it -- compile_module doesn't yet skip unreachable/unused
unsupported top-level constructs. Not attempted here: deciding what
"unused" means (reachability analysis) is a real design question, not a
quick fix.

## AOT: skip unreachable top-level code
A top-level `class`/`def` that nothing reachable from the statements which
actually run (always required, since they execute unconditionally) ever
refers to -- directly or transitively -- now gets skipped instead of
blocking the whole file. Reachability is computed generically (every
N_NAME reference found anywhere in the AST, recursively -- not a
kind-by-kind walker, so a future new node type can't silently make this
under-count) rather than listing node kinds by hand. A genuinely-used
class/function still correctly fails to compile exactly as before.

## 64-bit / portability audit
Static review (no ARM hardware or cross-compiler available in this
sandbox -- no network access to install one, so this is code review, not
compiled/run verification): no inline asm, no architecture `#ifdef`s, no
packed/aligned struct tricks, no pointer-to-int truncation (the few
pointer<->integer conversions that exist correctly go through `uintptr_t`
first). `Value`'s 16-byte layout is portable by construction (the union's
size is already forced to 8 bytes by its int64_t/double members
regardless of whether a pointer is 4 or 8 bytes on the target). One real
issue found and fixed: `read_file()` used `long` for file size (`fseek`/
`ftell`), which would silently truncate a file's size on a 32-bit-`long`
platform; changed to `int64_t` + `fseeko`/`ftello` with `_FILE_OFFSET_BITS
64` forced, so file size is handled correctly regardless of the target's
native `long` width. (Every call site updated to match -- a mismatched
`long*`/`int64_t*` pointer type at a call site would itself have been a
real bug on a 32-bit-`long` platform.)

## Self-hosted toolchain performance (measured, not assumed)
Checked whether the self-hosted lexer/parser/codegen (selfhost/*.cpy,
which run AS a cpy program, through the interpreter, when `aotc` compiles
something) are actually a bottleneck. They're not: an 800-line/200-function
file lexes+parses+codegens in ~34ms, and an 8000-line file in ~364ms --
linear scaling, no quadratic blowup. The *native* `cc -O2` step that turns
the generated C into a binary takes 80-140ms on the same file (mostly
linking against libcpyrt.a) -- 2-4x the self-hosted frontend's own cost.
So "optimize the self-hosted toolchain" wouldn't move the needle on `aotc`'s
actual wall-clock time for realistic inputs; the native compile/link step
dominates and is inherent to the AOT approach, not a self-hosting problem.
(selfhost/cpy.cpy, a *second* self-hosted tree-walking interpreter used
only by selfhost_test.sh to cross-check AOT output, is inherently slower
-- double interpretation -- but it's a testing tool, not something a user
runs, so its speed doesn't matter the same way.)

## Method dispatch inline cache
`obj.method(...)` for a builtin type (list/dict/str/tuple/set/...) went
through a dict hash lookup (list_methods etc.) on every single call, even
though that dict is fixed for the process's life. Each ATTR node used as
a method-call target now caches the type tag it last resolved for plus
the resolved method Value directly on the Node -- a hit skips the hash
lookup entirely. Deliberately excludes T_INST (user class instances):
method resolution there depends on the specific class (different classes
share the T_INST tag) AND on that individual instance's own attrs dict
(one instance can shadow a method with its own same-named attribute,
independently of other instances of the same class) -- caching by type
tag alone would be wrong there, so instances keep going through the full
lookup every time, unchanged. One shared cache implementation
(method_fn_cached) used by both the tree-walker (eval_call) and the VM
(VM_CALLM) rather than two copies that could drift apart.
Measured effect (not hypothesized from the earlier gprof numbers, which
turned out to be a less precise predictor than expected): modest, ~5-10%
on a tight `.append()` loop -- smaller than the gprof profile's dict_lookup/
str_hash call counts suggested, likely because -fno-inline profiling
(needed to get accurate call counts) also inflates per-call overhead
versus the real -O2 build. Kept anyway: it's a real, verified-correct,
zero-regression improvement, just a more modest one than initially
estimated -- reported honestly rather than oversold.
