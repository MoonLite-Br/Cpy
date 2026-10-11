# Packaging cpy

## User install options (today)

| Method | Command |
|--------|---------|
| From source | `./install.sh` |
| Debian/Ubuntu `.deb` | `sudo dpkg -i cpy_*.deb` (from Releases) |
| Termux | `./install.sh` or `dpkg -i cpy_*.deb` |

There is **no** official `apt install cpy` / `pkg install cpy` in distro
repos yet. That requires a maintainer upload (Debian) or a Termux PR.

## Build a `.deb` locally

```bash
sudo apt install build-essential libssl-dev dpkg-dev   # Debian/Ubuntu
# Termux: pkg install clang make openssl dpkg

./scripts/build-deb.sh 1.11.0
sudo dpkg -i build/cpy_1.11.0-1_*.deb
```

## Publish via GitHub Releases

```bash
git tag v1.11.0
git push origin v1.11.0
```

Workflows `build.yml` + `package.yml` attach binaries and `.deb` files to
the release.

## Path to `apt install cpy` / `pkg install cpy`

1. **GitHub Releases + `.deb`** (done by CI) — users download and `dpkg -i`
2. **Own APT repo** (e.g. on GitHub Pages / Cloudflare R2) with `reprepro`
3. **Debian mentors / experimental** — ITP + sponsor (months)
4. **Termux** — PR to [termux-packages](https://github.com/termux/termux-packages)
   using `packaging/termux/build.sh`
