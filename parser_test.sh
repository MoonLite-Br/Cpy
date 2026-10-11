#!/bin/sh
# Regression test for selfhost/parser.cpy (used by aotc): every file in
# tests/parser/ must at least PARSE successfully via aotc (reaching either
# a successful compile or a codegen-level rejection) -- a "syntax error"
# here means parser.cpy regressed and can no longer read a real-world
# construct it used to handle (see docs/vm.md's AOT section for the
# original gap this guards: try/except, with, raise, assert, del,
# nonlocal, from-import, lambda, is/is not/not in, await, yield/yield
# from, f-strings, keyword arguments at call sites).
cd "$(dirname "$0")" || exit 1
BIN=./bin/cpy
pass=0; fail=0
for f in tests/parser/t*.cpy; do
  out=$("$BIN" selfhost/aotc.cpy "$f" -o /tmp/cpy_parser_test_bin 2>&1)
  if echo "$out" | grep -q "syntax error"; then
    fail=$((fail+1)); echo "PARSE-FAIL $f"; echo "$out"
  else
    pass=$((pass+1)); echo "ok    $f"
  fi
done
rm -f /tmp/cpy_parser_test_bin
echo "parser battery: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
