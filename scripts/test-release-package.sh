#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <release-archive> <expected-package-version>" >&2
  exit 2
fi
archive="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"
expected_version="$2"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
host="$(uname -s 2>/dev/null || printf unknown)"
prefix="$tmp/prefix"
mkdir -p "$prefix"

case "$host" in
  Linux*|Darwin*)
    tar -xzf "$archive" -C "$tmp"
    package_root="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d -name 'cdin-kernel-*' -print -quit)"
    [[ -n "$package_root" ]] || { echo "package root not found in archive" >&2; exit 1; }
    [[ -x "$package_root/install.sh" ]] || { echo "install.sh missing or not executable" >&2; exit 1; }
    bash "$package_root/install.sh" --prefix "$prefix"
    EXPECTED_PACKAGE_VERSION="$expected_version" \
      bash "$repo_root/scripts/verify-installed.sh" "$prefix"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    command -v unzip >/dev/null 2>&1 || { echo "unzip is required to test the release ZIP" >&2; exit 2; }
    unzip -q "$archive" -d "$tmp"
    package_root="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d -name 'cdin-kernel-*' -print -quit)"
    [[ -n "$package_root" ]] || { echo "package root not found in archive" >&2; exit 1; }
    [[ -f "$package_root/install.ps1" ]] || { echo "install.ps1 missing from release ZIP" >&2; exit 1; }
    command -v powershell.exe >/dev/null 2>&1 || { echo "powershell.exe is required to test install.ps1" >&2; exit 2; }
    powershell.exe -NoProfile -ExecutionPolicy Bypass \
      -File "$(cygpath -w "$package_root/install.ps1")" \
      -Prefix "$(cygpath -w "$prefix")"
    [[ -f "$prefix/include/cdin/kernel.h" ]] || { echo "installed public header missing" >&2; exit 1; }
    [[ -f "$prefix/lib/libcdin_kernel.a" ]] || { echo "installed static library missing" >&2; exit 1; }
    [[ -f "$prefix/lib/libcdin_kernel.dll.a" ]] || { echo "installed import library missing" >&2; exit 1; }
    [[ -f "$prefix/bin/cdin_kernel.dll" ]] || { echo "installed runtime DLL missing" >&2; exit 1; }
    export PATH="$prefix/bin:$PATH"
    EXPECTED_PACKAGE_VERSION="$expected_version" \
      bash "$repo_root/scripts/verify-installed.sh" "$prefix"
    ;;
  *)
    echo "unsupported package-test host: $host" >&2
    exit 2
    ;;
esac

echo "release archive installation test: PASS ($host, package $expected_version)"
