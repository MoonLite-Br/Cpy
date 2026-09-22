#!/bin/sh
# Run the cpy test-suite.  Each tests/tNN_name.cpy is executed from inside tests/
# and its stdout is compared with tests/tNN_name.out.
#   ./run_tests.sh            run everything
#   ./run_tests.sh --update   regenerate the .out files (review them with git diff!)
cd "$(dirname "$0")" || exit 1
BIN=${CPY:-./bin/cpy}
[ -x "$BIN" ] || make >/dev/null || { echo "build failed"; exit 1; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")

pass=0; fail=0
cd tests || exit 1
for f in t*.cpy; do
  exp="${f%.cpy}.out"
  if [ "$1" = "--update" ]; then "$BIN" "$f" > "$exp" 2>/dev/null; echo "updated $exp"; continue; fi
  if "$BIN" "$f" > /tmp/cpy_test_out.$$ 2>/tmp/cpy_test_err.$$ && diff -u "$exp" /tmp/cpy_test_out.$$ > /tmp/cpy_test_diff.$$; then
    pass=$((pass+1)); echo "ok    $f"
  else
    fail=$((fail+1)); echo "FAIL  $f"; head -20 /tmp/cpy_test_diff.$$; head -5 /tmp/cpy_test_err.$$
  fi
done
rm -f /tmp/cpy_test_*.$$
[ "$1" = "--update" ] && exit 0

# --- examples must run cleanly ------------------------------------------------
cd ../examples || exit 1
for f in *.cpy; do
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
check "--version" "cpy 1.0.0" "$("$BIN" --version)"
"$BIN" -c 'raise ValueError("x")' >/dev/null 2>&1; check "uncaught exit code" "1" "$?"
"$BIN" -c 'def f(:' >/dev/null 2>&1; check "syntax error exit code" "1" "$?"
"$BIN" -c 'exit(7)' >/dev/null 2>&1; check "exit(7)" "7" "$?"
"$BIN" /nonexistent.cpy >/dev/null 2>&1; check "missing file exit code" "2" "$?"
check "uncaught message" "ValueError: boom" "$("$BIN" -c 'raise ValueError("boom")' 2>&1 | tail -1)"
check "traceback line" '  File "<string>", line 3, in f' "$("$BIN" -c 'def f():
    x = 1
    return 1 / 0
f()' 2>&1 | sed -n 3p)"
check "repl keeps state" "10" "$(printf 'def f(a):\n    return a * 2\n\nf(5)\n' | "$BIN" -i 2>/dev/null | grep -o '10$' | head -1)"
check "sys.argv" "3 b" "$(printf 'import sys\nprint(len(sys.argv), sys.argv[2])\n' > /tmp/argv_t.cpy; "$BIN" /tmp/argv_t.cpy a b)"

echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
