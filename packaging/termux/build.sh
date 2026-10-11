# Termux package recipe sketch (for termux-packages repo).
# Place as packages/cpy/build.sh after forking termux/termux-packages.
TERMUX_PKG_HOMEPAGE=https://github.com/MoonLite-Br/Cpy
TERMUX_PKG_DESCRIPTION="Python-syntax interpreter written in portable C"
TERMUX_PKG_LICENSE="MIT"
TERMUX_PKG_MAINTAINER="@MoonLite-Br"
TERMUX_PKG_VERSION=1.11.0
TERMUX_PKG_SRCURL=https://github.com/MoonLite-Br/Cpy/archive/refs/tags/v${TERMUX_PKG_VERSION}.tar.gz
TERMUX_PKG_SHA256=SKIP_UNTIL_RELEASE
TERMUX_PKG_DEPENDS="openssl"
TERMUX_PKG_BUILD_IN_SRC=true

termux_step_make() {
	make CC="$CC" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"
}

termux_step_make_install() {
	install -Dm700 bin/cpy "$TERMUX_PREFIX/bin/cpy"
	mkdir -p "$TERMUX_PREFIX/share/cpy/selfhost"
	cp -a selfhost/. "$TERMUX_PREFIX/share/cpy/selfhost/"
	cat > "$TERMUX_PREFIX/bin/aotc" <<-WRAP
	#!/data/data/com.termux/files/usr/bin/sh
	exec $TERMUX_PREFIX/bin/cpy $TERMUX_PREFIX/share/cpy/selfhost/aotc.cpy "\$@"
	WRAP
	chmod 700 "$TERMUX_PREFIX/bin/aotc"
}
