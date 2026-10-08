#!/bin/sh
# Build cpy -> bin/cpy  (needs a C compiler: on Termux run `pkg install clang make`)
set -e
cd "$(dirname "$0")"
make
echo "OK -> bin/cpy"
