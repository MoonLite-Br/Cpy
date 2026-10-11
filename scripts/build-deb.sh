#!/bin/sh
# Build a .deb for cpy (Debian/Ubuntu/Termux). From repo root:
#   ./scripts/build-deb.sh [version]
set -e
cd "$(dirname "$0")/.."
VER="${1:-1.11.0}"
TMP="${TMPDIR:-/tmp}/cpy-deb-$$"

make clean 2>/dev/null || true
make
test -x bin/cpy

rm -rf "$TMP"
mkdir -p "$TMP/DEBIAN" "$TMP/usr/bin" "$TMP/usr/share/cpy/selfhost" "$TMP/usr/share/doc/cpy"

install -m 0755 bin/cpy "$TMP/usr/bin/cpy"
cp -a selfhost/. "$TMP/usr/share/cpy/selfhost/"
printf '#!/bin/sh\nexec /usr/bin/cpy /usr/share/cpy/selfhost/aotc.cpy "$@"\n' > "$TMP/usr/bin/aotc"
chmod 0755 "$TMP/usr/bin/aotc"

cp LICENSE "$TMP/usr/share/doc/cpy/copyright" 2>/dev/null || true
printf 'cpy (%s-1) unstable; urgency=medium\n\n  * Package upstream %s\n\n -- moonlite-admin <moonlite-admin@users.noreply.github.com>  %s\n' \
  "$VER" "$VER" "$(date -R)" > "$TMP/usr/share/doc/cpy/changelog.Debian"
gzip -9nf "$TMP/usr/share/doc/cpy/changelog.Debian"

ARCH=$(dpkg --print-architecture 2>/dev/null || uname -m)
case "$ARCH" in x86_64) ARCH=amd64 ;; aarch64) ARCH=arm64 ;; esac
SIZE=$(du -sk "$TMP" | cut -f1)

DEPS="libc6"
if ldd bin/cpy 2>/dev/null | grep -q libssl; then
  DEPS="$DEPS, libssl3 | libssl1.1"
fi

cat > "$TMP/DEBIAN/control" <<CTL
Package: cpy
Version: $VER-1
Section: interpreters
Priority: optional
Architecture: $ARCH
Maintainer: moonlite-admin <moonlite-admin@users.noreply.github.com>
Depends: $DEPS
Installed-Size: $SIZE
Homepage: https://github.com/MoonLite-Br/Cpy
Description: Python-syntax interpreter written in portable C
 cpy runs Python-like programs through a from-scratch interpreter in C.
 One binary, optional HTTPS via OpenSSL, Termux-friendly.
CTL

mkdir -p build
OUT="build/cpy_${VER}-1_${ARCH}.deb"
# dpkg-deb writes more reliably under /tmp on some FS
dpkg-deb --root-owner-group --build "$TMP" "/tmp/cpy_${VER}-1_${ARCH}.deb"
cp "/tmp/cpy_${VER}-1_${ARCH}.deb" "$OUT"
rm -rf "$TMP" "/tmp/cpy_${VER}-1_${ARCH}.deb"
ls -la "$OUT"
echo "Install: sudo dpkg -i $OUT"
