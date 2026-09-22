#!/bin/sh
# Build and install cpy.
#   Termux:  installs to $PREFIX/bin      Linux/macOS: /usr/local/bin (if writable) or ~/bin
set -e
cd "$(dirname "$0")"

CC=${CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "No C compiler found."
  echo "On Termux run:   pkg install clang make"
  echo "On Debian/Ubuntu: sudo apt install build-essential"
  exit 1
fi

make CC="$CC"
chmod +x bin/cpy

if [ -n "$PREFIX" ] && [ -d "$PREFIX/bin" ]; then
  cp bin/cpy "$PREFIX/bin/cpy"
  echo "Installed: $PREFIX/bin/cpy"
elif [ -w /usr/local/bin ] 2>/dev/null; then
  cp bin/cpy /usr/local/bin/cpy
  echo "Installed: /usr/local/bin/cpy"
else
  mkdir -p "$HOME/bin"
  cp bin/cpy "$HOME/bin/cpy"
  echo "Installed: $HOME/bin/cpy"
  case ":$PATH:" in
    *":$HOME/bin:"*) ;;
    *) echo 'export PATH="$HOME/bin:$PATH"' >> "$HOME/.bashrc"
       echo "Run: source ~/.bashrc" ;;
  esac
fi
echo "Use: cpy   or   cpy file.cpy"
