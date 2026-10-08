#!/bin/sh
# Developer helper: run every test with real CPython and with cpy, diff the output.
# (cpy modules use the .cpy extension, so copy them to .py for CPython.)
cd "$(dirname "$0")" || exit 1
for m in helper_mod; do [ -f $m.cpy ] && cp $m.cpy $m.py; done
fail=0
for f in t*.cpy; do
  python3 "$f" > /tmp/cpy_ref.out 2>/dev/null
  ../bin/cpy "$f" > /tmp/cpy_got.out 2>/dev/null
  if diff -q /tmp/cpy_ref.out /tmp/cpy_got.out >/dev/null; then echo "same  $f"; else echo "DIFF  $f"; diff /tmp/cpy_ref.out /tmp/cpy_got.out | head -10; fail=1; fi
done
rm -f helper_mod.py; rm -rf __pycache__
exit $fail
