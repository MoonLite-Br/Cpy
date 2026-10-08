#!/usr/bin/env cpy
# selfhost/aotc.cpy - AOT compiler driver: cpy source -> native executable.
#
#   aotc in.cpy -o out && ./out              (after ./install.sh, "aotc" is on your PATH)
#   cpy selfhost/aotc.cpy in.cpy -o out       (works from a plain checkout too)
#
# Runs the self-hosted lexer/parser + codegen.cpy to produce C, then shells
# out to `cc` to turn that C into a real machine-code binary, linked against
# the same runtime (bin/libcpyrt.a) the tree-walking interpreter uses.
#
# Scope: numeric cpy only (int/float/bool/None, arithmetic, if/while/for
# range(...) loops, plain functions, and `import`ing real modules like math
# or random). No strings, lists, dicts, or classes yet -- see
# selfhost/README.md for why, and what a CompileError from this tool means.
import sys
import os
from codegen import compile_source, CompileError
from lexer import LexError
from parser import ParseError

SELF_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SELF_DIR)

USAGE = """usage: aotc <in.cpy> [-o out] [--run] [--keep-c] [--cc=clang]

  <in.cpy>     the cpy script to compile (numeric subset only -- see
               selfhost/README.md for exactly what that covers)
  -o out       name of the native binary to write (default: <in> without
               its extension, in the current directory)
  --run        run the compiled binary immediately after building it
  --keep-c     keep the generated <out>.aot.c file instead of deleting it
               (useful for seeing exactly what got generated)
  --cc=CC      C compiler to use (default: $CC, or "cc")

examples:
  aotc fib.cpy -o fib && ./fib
  aotc fib.cpy --run
  aotc fib.cpy --keep-c -o fib   # then look at fib.aot.c

If a script uses something outside the numeric subset (strings, lists,
classes, ...), aotc reports a CompileError with the source line and exactly
which construct isn't supported yet -- that script just needs `cpy` itself
(the tree-walking interpreter), which has no such restriction."""


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(USAGE)
        sys.exit(0 if args else 2)
    src_path = None
    out_path = None
    keep_c = False
    run_after = False
    cc = os.getenv("CC", "cc")
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            i += 1
            if i >= len(args):
                print("aotc: -o needs an argument")
                sys.exit(2)
            out_path = args[i]
        elif a == "--keep-c":
            keep_c = True
        elif a == "--run":
            run_after = True
        elif a.startswith("--cc="):
            cc = a[5:]
        elif a.startswith("-"):
            print(f"aotc: unknown option {a!r}")
            print(USAGE)
            sys.exit(2)
        elif src_path is None:
            src_path = a
        else:
            print(f"aotc: unexpected extra argument {a!r} (already have input {src_path!r})")
            sys.exit(2)
        i += 1
    if not src_path:
        print("aotc: no input file")
        print(USAGE)
        sys.exit(2)
    if not os.path.exists(src_path):
        print(f"aotc: no such file: {src_path}")
        sys.exit(2)
    if not out_path:
        base = os.path.basename(src_path)
        out_path = base[:-4] if base.endswith(".cpy") else (base[:-3] if base.endswith(".py") else base + ".out")

    with open(src_path) as f:
        src = f.read()
    try:
        c_src = compile_source(src, src_path)
    except (LexError, ParseError) as e:
        print(f"aotc: {src_path}: syntax error: {e}")
        sys.exit(1)
    except CompileError as e:
        print(f"aotc: {src_path}: {e}")
        print("      (this construct needs the regular interpreter: cpy " + src_path + ")")
        sys.exit(1)

    c_path = out_path + ".aot.c"
    with open(c_path, "w") as f:
        f.write(c_src)

    lib = ROOT + "/bin/libcpyrt.a"
    if not os.path.exists(lib):
        print("aotc: building bin/libcpyrt.a (one-time)...")
        rc = os.system(f"make -C {ROOT} bin/libcpyrt.a")
        if rc != 0:
            print("aotc: failed to build the runtime library -- see the error above")
            sys.exit(1)

    cmd = f"{cc} -O2 -I{ROOT}/src -o {out_path} {c_path} {lib} -lm -pthread"
    rc = os.system(cmd)
    if not keep_c:
        os.remove(c_path)
    if rc != 0:
        print("aotc: cc failed to compile the generated C -- this is likely a bug in")
        print("      aotc itself, not your script; please report it. Re-run with")
        print("      --keep-c to inspect the generated C file.")
        sys.exit(1)
    print(f"aotc: wrote {out_path}")
    if run_after:
        run_cmd = out_path if out_path.startswith("/") or out_path.startswith("./") else "./" + out_path
        print(f"aotc: running {run_cmd}")
        sys.exit(os.system(run_cmd) >> 8)


main()
