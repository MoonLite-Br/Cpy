# Termux package for cpy

## Option A — local `.deb` on device

```bash
pkg install clang make openssl dpkg
./scripts/build-deb.sh          # produces build/cpy_*.deb
dpkg -i build/cpy_*.deb
```

Termux uses `$PREFIX` (`/data/data/com.termux/files/usr`). The simple
`scripts/build-deb.sh` installs to `/usr/bin` which is correct **inside**
the Termux prefix when `dpkg` is the Termux one.

## Option B — official `pkg install cpy` (long path)

1. Fork https://github.com/termux/termux-packages  
2. Add `packages/cpy/build.sh` (see `build.sh` in this folder)  
3. Open a PR; maintainers review build on CI  

Until merged, users install from GitHub Releases `.deb` or `./install.sh`.
