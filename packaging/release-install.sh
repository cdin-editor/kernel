#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: ./install.sh [--prefix PATH]

Installs cdin-kernel from this release archive. The default is ~/.local, so
root privileges are not required. Use --prefix /usr/local for a system-wide
installation (run with sudo if needed).
USAGE
}

prefix="${HOME:?HOME must be set}/.local"
while (($#)); do
  case "$1" in
    --prefix|-p)
      if (($# < 2)) || [[ -z "$2" ]]; then
        echo "--prefix requires a path" >&2
        exit 2
      fi
      prefix="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

package_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
for required in "$package_root/include/cdin/kernel.h" "$package_root/lib/libcdin_kernel.a" "$package_root/lib/pkgconfig/cdin-kernel.pc"; do
  if [[ ! -f "$required" ]]; then
    echo "release archive is incomplete; missing $required" >&2
    exit 1
  fi
done

mkdir -p "$prefix/include" "$prefix/lib"
cp -a "$package_root/include/." "$prefix/include/"
cp -a "$package_root/lib/." "$prefix/lib/"
if [[ -d "$package_root/bin" ]]; then
  mkdir -p "$prefix/bin"
  cp -a "$package_root/bin/." "$prefix/bin/"
fi
if [[ -d "$package_root/share" ]]; then
  mkdir -p "$prefix/share"
  cp -a "$package_root/share/." "$prefix/share/"
fi

host="$(uname -s 2>/dev/null || printf unknown)"
if [[ "$host" == Linux* && "$prefix" == /usr/local && "$(id -u)" == 0 ]] && command -v ldconfig >/dev/null 2>&1; then
  ldconfig
fi

printf 'Installed cdin-kernel to %s\n' "$prefix"
printf 'Header: %s/include/cdin/kernel.h\n' "$prefix"
printf 'Static library: %s/lib/libcdin_kernel.a\n' "$prefix"
printf 'For pkg-config, set: PKG_CONFIG_PATH="%s/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"\n' "$prefix"
printf 'Package version: '
awk -F': ' '/^Package version:/ {print $2; found=1; exit} END {if (!found) print "unknown"}' "$package_root/BUILD-INFO.txt" 2>/dev/null || true
