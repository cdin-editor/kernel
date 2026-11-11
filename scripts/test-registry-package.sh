#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <registry-prefix-archive> <expected-package-version>" >&2
  exit 2
fi
archive="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
expected_version="$2"
repo_root="$(cd "$(dirname "\${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
host="$(uname -s 2>/dev/null || printf unknown)"
package_root="$tmp/package"
prefix="$tmp/prefix"
mkdir -p "$package_root" "$prefix"

case "$host" in
  Linux*|Darwin*)
    tar -xzf "$archive" -C "$package_root"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    command -v unzip >/dev/null 2>&1 || { echo "unzip is required to test the Registry ZIP" >&2; exit 2; }
    unzip -q "$archive" -d "$package_root"
    ;;
  *)
    echo "unsupported package-test host: $host" >&2
    exit 2
    ;;
esac

# cmake-prefix-v1 is the actual install prefix, not a wrapper directory.
for required in \
  BUILD-INFO.txt \
  include/cdin/kernel.h \
  include/m-mdy-m/log.h \
  lib/libcdin_kernel.a \
  lib/libm-mdy-m-log.a \
  lib/pkgconfig/cdin-kernel.pc \
  lib/pkgconfig/m-mdy-m-log.pc; do
  [[ -f "$package_root/$required" ]] || {
    echo "Registry package is not rooted at the install prefix; missing $required" >&2
    exit 1
  }
done

build_version="$(awk -F': ' '/^Package version:/ {print $2; found=1; exit} END {if (!found) exit 1}' "$package_root/BUILD-INFO.txt")"
[[ "$build_version" == "$expected_version" ]] || {
  echo "Registry package BUILD-INFO version is $build_version; expected $expected_version" >&2
  exit 1
}

case "$host" in
  Linux*|Darwin*)
    [[ -x "$package_root/install.sh" ]] || {
      echo "Registry package install.sh is missing or not executable" >&2
      exit 1
    }
    (cd "$package_root" && bash ./install.sh --prefix "$prefix")
    ;;
  MINGW*|MSYS*|CYGWIN*)
    [[ -f "$package_root/install.ps1" ]] || {
      echo "Registry package install.ps1 is missing" >&2
      exit 1
    }
    command -v powershell.exe >/dev/null 2>&1 || {
      echo "powershell.exe is required to test install.ps1" >&2
      exit 2
    }
    powershell.exe -NoProfile -ExecutionPolicy Bypass \
      -File "$(cygpath -w "$package_root/install.ps1")" \
      -Prefix "$(cygpath -w "$prefix")"
    export PATH="$prefix/bin:$PATH"
    ;;
esac

EXPECTED_PACKAGE_VERSION="$expected_version" \
  bash "$repo_root/scripts/verify-installed.sh" "$prefix"

echo "Registry cmake-prefix-v1 installation test: PASS ($host, package $expected_version)"
