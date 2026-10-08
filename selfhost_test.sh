#!/bin/bash
# Compare selfhost/cpy.cpy (tree-walking) and, where in scope, selfhost's AOT
# compiler against the native interpreter, for every script in
# selfhost/tests/. Run from the repo root.
cd "$(dirname "$0")" || exit 1
BIN=${CPY:-./bin/cpy}
pass=0; fail=0
for f in selfhost/tests/t*.cpy; do
  a=$("$BIN" "$f" 2>&1)
  if grep -q '^import ' "$f"; then
    echo "skip  tree-walk $f (uses import -- tree-walking selfhost/cpy.cpy doesn't support it, only AOT does; see selfhost/README.md)"
  else
    b=$("$BIN" selfhost/cpy.cpy "$f" 2>&1)
    if [ "$a" = "$b" ]; then pass=$((pass+1)); echo "ok    tree-walk $f"; else fail=$((fail+1)); echo "FAIL  tree-walk $f"; diff <(echo "$a") <(echo "$b"); fi
  fi

  out=$(mktemp -u)
  buildlog=$("$BIN" selfhost/aotc.cpy "$f" -o "$out" 2>&1)
  if echo "$buildlog" | grep -q "aotc: wrote"; then
    c=$("$out" 2>&1)
    rm -f "$out"
    if [ "$a" = "$c" ]; then pass=$((pass+1)); echo "ok    AOT       $f"; else fail=$((fail+1)); echo "FAIL  AOT       $f"; diff <(echo "$a") <(echo "$c"); fi
  else
    echo "skip  AOT       $f (not in the numeric subset -- expected for most of these)"
  fi
done
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
