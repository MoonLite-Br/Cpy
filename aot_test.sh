#!/bin/sh
cd "$(dirname "$0")" || exit 1
BIN=./bin/cpy
pass=0; fail=0
for f in tests/aot/t_*.cpy; do
  i_out=$(CPY_NOVM=1 "$BIN" "$f" 2>&1)
  i_exit=$?
  "$BIN" selfhost/aotc.cpy "$f" -o /tmp/aot_check_bin > /tmp/aot_check_log 2>&1
  if ! grep -q "aotc: wrote" /tmp/aot_check_log; then
    echo "COMPILE-FAIL $f"; cat /tmp/aot_check_log; fail=$((fail+1)); continue
  fi
  a_out=$(/tmp/aot_check_bin 2>&1)
  a_exit=$?
  i_last=$(printf '%s' "$i_out" | tail -1)
  a_last=$(printf '%s' "$a_out" | tail -1)
  if [ "$i_exit" = "$a_exit" ] && [ "$i_last" = "$a_last" ]; then
    pass=$((pass+1)); echo "ok    $f"
  else
    fail=$((fail+1)); echo "FAIL  $f"
    echo "  interp[$i_exit]: $i_out" | head -5
    echo "  aot[$a_exit]:    $a_out" | head -5
  fi
done
rm -f /tmp/aot_check_bin /tmp/aot_check_log
echo "aot battery: passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
