#!/bin/sh
# Build and install cpy.
#   Termux:  installs to $PREFIX/bin      Linux/macOS: /usr/local/bin (if writable) or ~/bin
set -e
cd "$(dirname "$0")"
ROOT="$(pwd)"

CC=${CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
  echo "No C compiler found."
  echo "On Termux run:   pkg install clang make"
  echo "On Debian/Ubuntu: sudo apt install build-essential"
  exit 1
fi

make CC="$CC"
chmod +x bin/cpy

# Build the runtime library too, so the AOT compiler (`aotc`, installed
# below) can compile cpy scripts to native binaries right away.
make CC="$CC" aot || echo "note: 'make aot' failed; the interpreter above still works fine, only 'aotc' needs bin/libcpyrt.a"

if [ -n "$PREFIX" ] && [ -d "$PREFIX/bin" ]; then
  BINDIR="$PREFIX/bin"
elif [ -w /usr/local/bin ] 2>/dev/null; then
  BINDIR="/usr/local/bin"
else
  BINDIR="$HOME/bin"
  mkdir -p "$BINDIR"
  case ":$PATH:" in
    *":$BINDIR:"*) ;;
    *) echo "export PATH=\"$BINDIR:\$PATH\"" >> "$HOME/.bashrc"
       echo "Added $BINDIR to PATH in ~/.bashrc -- run: source ~/.bashrc" ;;
  esac
fi

cp bin/cpy "$BINDIR/cpy"
cat > "$BINDIR/aotc" <<WRAPPER
#!/bin/sh
# Compiles numeric cpy scripts to native machine code. See: aotc --help
exec "$BINDIR/cpy" "$ROOT/selfhost/aotc.cpy" "\$@"
WRAPPER
chmod +x "$BINDIR/aotc"

echo "Installed: $BINDIR/cpy, $BINDIR/aotc"
echo "Use: cpy                 -- REPL"
echo "     cpy file.cpy         -- run a script"
echo "     aotc in.cpy -o out   -- compile a numeric script to a native binary, then run ./out"
