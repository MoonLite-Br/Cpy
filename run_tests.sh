#!/bin/sh
# Run the cpy test-suite.  Each tests/tNN_name.cpi is executed from inside tests/
# and its stdout is compared with tests/tNN_name.out.
#   ./run_tests.sh            run everything
#   ./run_tests.sh --update   regenerate the .out files (review them with git diff!)
cd "$(dirname "$0")" || exit 1
# Scratch dir: Termux has no writable /tmp (it uses $PREFIX/tmp), so honour $TMPDIR and fall back
# to a directory next to the repo if mktemp is unavailable.
T=$(mktemp -d 2>/dev/null) || { T="${TMPDIR:-.}/cpy_test_$$"; mkdir -p "$T" || exit 1; }
trap 'rm -rf "$T"' EXIT
BIN=${CPY:-./bin/cpy}
[ -x "$BIN" ] || make >/dev/null || { echo "build failed"; exit 1; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

pass=0; fail=0
cd tests || exit 1
for f in t*.cpi; do
  [ -f "$f" ] || continue
  exp="${f%.cpi}.out"
  if [ "$1" = "--update" ]; then "$BIN" "$f" > "$exp" 2>/dev/null; echo "updated $exp"; continue; fi
  if [ ! -f "$exp" ]; then
    fail=$((fail+1)); echo "FAIL  $f (missing $exp)"; continue
  fi
  if "$BIN" "$f" > $T/out 2>$T/err && diff -u "$exp" $T/out > $T/diff; then
    pass=$((pass+1)); echo "ok    $f"
  else
    fail=$((fail+1)); echo "FAIL  $f"; head -20 $T/diff 2>/dev/null; head -5 $T/err 2>/dev/null
  fi
done
[ "$1" = "--update" ] && exit 0

# --- examples must run cleanly ------------------------------------------------
cd ../examples || exit 1
for f in *.cpi; do
  [ -f "$f" ] || continue
  if "$BIN" "$f" >/dev/null 2>&1 </dev/null; then pass=$((pass+1)); echo "ok    example: $f"; else fail=$((fail+1)); echo "FAIL  example: $f"; fi
done
rm -f demo_out.txt
cd ../tests || exit 1

# --- command line behaviour ---------------------------------------------------
check() { # name, expected, actual
  if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "ok    cli: $1"; else fail=$((fail+1)); echo "FAIL  cli: $1 (expected '$2', got '$3')"; fi
}
check "-c" "5050" "$("$BIN" -c 'print(sum(range(101)))')"
check "stdin script" "42" "$(echo 'print(6 * 7)' | "$BIN")"
check "--version" "cpy 1.10.0" "$("$BIN" --version)"
"$BIN" -c 'raise ValueError("x")' >/dev/null 2>&1; check "uncaught exit code" "1" "$?"
"$BIN" -c 'def f(:' >/dev/null 2>&1; check "syntax error exit code" "1" "$?"
"$BIN" -c 'exit(7)' >/dev/null 2>&1; check "exit(7)" "7" "$?"
"$BIN" /nonexistent.cpi >/dev/null 2>&1; check "missing file exit code" "2" "$?"
check "uncaught message" "ValueError: boom" "$("$BIN" -c 'raise ValueError("boom")' 2>&1 | tail -1)"
check "traceback line" '  File "<string>", line 3, in f' "$("$BIN" -c 'def f():
    x = 1
    return 1 / 0
f()' 2>&1 | sed -n 3p)"
check "repl keeps state" "10" "$(printf 'def f(a):\n    return a * 2\n\nf(5)\n' | "$BIN" -i 2>/dev/null | grep -o '10$' | head -1)"
check "sys.argv" "3 b" "$(printf 'import sys\nprint(len(sys.argv), sys.argv[2])\n' > $T/argv_t.cpi; "$BIN" $T/argv_t.cpi a b)"

# --- the bytecode VM must agree with the tree-walker on every test ------------
for f in t*.cpi; do
  [ -f "$f" ] || continue
  "$BIN" --no-vm "$f" > $T/vm_a 2>/dev/null; ra=$?
  "$BIN" "$f" > $T/vm_b 2>/dev/null; rb=$?
  if [ $ra -eq $rb ] && cmp -s $T/vm_a $T/vm_b; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL  vm vs tree-walker: $f"; fi
done

# Optional fuzz / aot / parser suites (skip if scripts not present)
PYBIN=$(command -v python3 || true)
if [ -n "$PYBIN" ] && [ -f vm_fuzz.py ]; then
  vf=$("$PYBIN" vm_fuzz.py 150 777 2>&1 | tail -1)
  case "$vf" in *"mismatches: 0"*) pass=$((pass+1));; *) fail=$((fail+1)); echo "FAIL  vm_fuzz.py: $vf";; esac
fi
if [ -n "$PYBIN" ] && [ -f vm_toplevel_fuzz.py ]; then
  tf=$("$PYBIN" vm_toplevel_fuzz.py 150 777 2>&1 | tail -1)
  case "$tf" in *"mismatches: 0"*) pass=$((pass+1));; *) fail=$((fail+1)); echo "FAIL  vm_toplevel_fuzz.py: $tf";; esac
fi
cd ..
if [ -f aot_test.sh ]; then
  af=$(sh aot_test.sh 2>&1)
  case "$af" in *"failed=0"*) pass=$((pass+1));; *) fail=$((fail+1)); echo "FAIL  aot_test.sh:"; echo "$af";; esac
fi
if [ -f parser_test.sh ]; then
  pf=$(sh parser_test.sh 2>&1)
  case "$pf" in *"failed=0"*) pass=$((pass+1));; *) fail=$((fail+1)); echo "FAIL  parser_test.sh:"; echo "$pf";; esac
fi
cd tests || exit 1
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
