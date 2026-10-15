#!/bin/sh
# cpy package helper — called by `cpy install|list|uninstall`
# Layout:
#   $CPY_HOME/site/<name>/     installed package (importable)
#   $CPY_HOME/cache/           clones / tar.gz
#   $CPY_HOME/index            name|version|source
set -e

cmd="${1:-}"
shift 2>/dev/null || true

cpy_home() {
  if [ -n "${CPY_HOME-}" ]; then
    echo "$CPY_HOME"
  elif [ -n "${PREFIX-}" ] && [ -d "$PREFIX" ]; then
    echo "$PREFIX/share/cpy"
  else
    echo "${HOME:-/tmp}/.local/share/cpy"
  fi
}

HOME_DIR="$(cpy_home)"
SITE="$HOME_DIR/site"
CACHE="$HOME_DIR/cache"
INDEX="$HOME_DIR/index"
mkdir -p "$SITE" "$CACHE"

need_git() {
  if ! command -v git >/dev/null 2>&1; then
    echo "cpy: git not found. Termux: pkg install git   Debian: apt install git" >&2
    exit 1
  fi
}

# Find a directory that looks like a cpy package; print "name|path"
detect_pkg() {
  root="$1"
  # manifest
  if [ -f "$root/cpy-package.toml" ]; then
    name=$(grep -E '^name\s*=' "$root/cpy-package.toml" | head -1 | sed 's/.*=\s*"\?\([^"]*\)"\?.*/\1/' | tr -d ' ')
    pkgdir=$(grep -E '^package\s*=' "$root/cpy-package.toml" | head -1 | sed 's/.*=\s*"\?\([^"]*\)"\?.*/\1/' | tr -d ' ')
    [ -z "$pkgdir" ] && pkgdir="$name"
    if [ -n "$name" ] && [ -d "$root/$pkgdir" ] && { [ -f "$root/$pkgdir/__init__.cpy" ] || [ -f "$root/$pkgdir/__init__.py" ]; }; then
      echo "$name|$root/$pkgdir"
      return 0
    fi
  fi
  # htmlx/ / cpypi/ style
  for d in "$root"/*; do
    [ -d "$d" ] || continue
    base=$(basename "$d")
    case "$base" in .git|tests|examples|docs|src|tools|bench|selfhost) continue ;; esac
    if [ -f "$d/__init__.cpy" ] || [ -f "$d/__init__.py" ]; then
      echo "$base|$d"
      return 0
    fi
  done
  # root is the package
  if [ -f "$root/__init__.cpy" ] || [ -f "$root/__init__.py" ]; then
    name=$(basename "$root")
    echo "$name|$root"
    return 0
  fi
  return 1
}

cmd_list() {
  if [ ! -f "$INDEX" ] || [ ! -s "$INDEX" ]; then
    echo "(no packages installed)"
    echo "site: $SITE"
    return 0
  fi
  printf "%-16s %-12s %s\n" "NAME" "VERSION" "SOURCE"
  while IFS='|' read -r name ver src; do
    [ -n "$name" ] || continue
    printf "%-16s %-12s %s\n" "$name" "$ver" "$src"
  done < "$INDEX"
}

cmd_uninstall() {
  name="$1"
  if [ -z "$name" ]; then
    echo "usage: cpy uninstall <name>" >&2
    exit 2
  fi
  rm -rf "$SITE/$name"
  if [ -f "$INDEX" ]; then
    grep -v "^${name}|" "$INDEX" > "$INDEX.tmp" 2>/dev/null || true
    mv "$INDEX.tmp" "$INDEX"
  fi
  echo "removed $name"
}

index_set() {
  name="$1"; ver="$2"; src="$3"
  touch "$INDEX"
  grep -v "^${name}|" "$INDEX" > "$INDEX.tmp" 2>/dev/null || true
  echo "${name}|${ver}|${src}" >> "$INDEX.tmp"
  mv "$INDEX.tmp" "$INDEX"
}

cmd_install() {
  spec="$1"
  if [ -z "$spec" ]; then
    echo "usage: cpy install <git-url|local-path>" >&2
    exit 2
  fi

  work="$CACHE/work-$$"
  rm -rf "$work"
  mkdir -p "$work"

  if [ -d "$spec" ]; then
    # local path
    srcpath=$(CDPATH= cd -- "$spec" && pwd)
    src_label="$srcpath"
    cp -a "$srcpath" "$work/src"
  else
    need_git
    url="$spec"
    # normalize github without .git
    echo "cloning $url ..."
    if ! git clone --depth 1 "$url" "$work/src" 2>"$work/err"; then
      # try adding .git
      if ! git clone --depth 1 "${url}.git" "$work/src" 2>>"$work/err"; then
        echo "cpy: git clone failed:" >&2
        cat "$work/err" >&2
        rm -rf "$work"
        exit 1
      fi
      url="${url}.git"
    fi
    src_label="$url"
  fi

  if ! info=$(detect_pkg "$work/src"); then
    echo "cpy: not a valid cpy package (need <name>/__init__.cpy or cpy-package.toml)" >&2
    rm -rf "$work"
    exit 1
  fi
  name=$(echo "$info" | cut -d'|' -f1)
  pkgpath=$(echo "$info" | cut -d'|' -f2)

  ver="0.0.0"
  if [ -f "$pkgpath/__init__.cpy" ]; then
    v=$(grep -E '__version__\s*=' "$pkgpath/__init__.cpy" | head -1 | sed 's/.*"\([^"]*\)".*/\1/')
    [ -n "$v" ] && ver="$v"
  fi

  # pack tar.gz cache
  tarball="$CACHE/${name}-${ver}.tar.gz"
  tar -czf "$tarball" -C "$(dirname "$pkgpath")" "$(basename "$pkgpath")" 2>/dev/null || true

  # install into site
  rm -rf "$SITE/$name"
  mkdir -p "$SITE/$name"
  cp -a "$pkgpath"/. "$SITE/$name/"

  index_set "$name" "$ver" "$src_label"
  rm -rf "$work"

  echo "installed $name $ver"
  echo "  -> $SITE/$name"
  echo "  import with:  cpy -c 'import $name'"
  echo "  (site path is auto-loaded; no CPY_PATH needed)"
}

case "$cmd" in
  install) cmd_install "$@" ;;
  list) cmd_list ;;
  uninstall) cmd_uninstall "$@" ;;
  home) echo "$HOME_DIR" ;;
  *)
    echo "usage: cpy install <git-url|path>" >&2
    echo "       cpy list" >&2
    echo "       cpy uninstall <name>" >&2
    exit 2
    ;;
esac
