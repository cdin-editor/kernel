#!/usr/bin/env bash
set -euo pipefail

: "${PACKAGE_VERSION:?set PACKAGE_VERSION to the package version}"
dist="${1:-dist}"

[[ -d "$dist" ]] || { echo "release output directory does not exist: $dist" >&2; exit 2; }

host="$(uname -s 2>/dev/null || printf unknown)"
arch="$(uname -m 2>/dev/null || printf unknown)"
case "$host" in
  Linux*) platform=linux ;;
  Darwin*) platform=macos ;;
  MINGW*|MSYS*|CYGWIN*) platform=windows ;;
  *) echo "unsupported packaging host: $host" >&2; exit 2 ;;
esac
case "$arch" in
  amd64|x64) arch=x86_64 ;;
  aarch64) arch=arm64 ;;
esac

asset_base="cdin-kernel-${PACKAGE_VERSION}-${platform}-${arch}"
if [[ "$platform" == windows ]]; then
  archive_name="${asset_base}.zip"
else
  archive_name="${asset_base}.tar.gz"
fi
checksum_name="${archive_name}.sha256"

# Fail closed: a target must contribute exactly one archive and its matching
# checksum. Loose files and directories must never become release assets.
shopt -s nullglob
entries=("$dist"/*)
if [[ ${#entries[@]} -ne 2 ]]; then
  printf 'expected exactly 2 release output files in %s, found %s\n' "$dist" "${#entries[@]}" >&2
  printf '  %s\n' "${entries[@]}" >&2
  exit 1
fi

archive_found=0
checksum_found=0
for entry in "${entries[@]}"; do
  if [[ ! -f "$entry" || -L "$entry" ]]; then
    echo "release output must contain regular files only: $entry" >&2
    exit 1
  fi
  case "$(basename "$entry")" in
    "$archive_name") archive_found=1 ;;
    "$checksum_name") checksum_found=1 ;;
    *) echo "unexpected release output: $entry" >&2; exit 1 ;;
  esac
done

[[ "$archive_found" -eq 1 && "$checksum_found" -eq 1 ]] || {
  echo "missing expected archive or checksum for ${platform}/${arch}" >&2
  exit 1
}

if command -v sha256sum >/dev/null 2>&1; then
  (cd "$dist" && sha256sum -c "$checksum_name")
elif command -v shasum >/dev/null 2>&1; then
  (cd "$dist" && shasum -a 256 -c "$checksum_name")
else
  echo 'neither sha256sum nor shasum is available to verify the release archive' >&2
  exit 2
fi

printf 'Release output verified: %s and %s\n' "$archive_name" "$checksum_name"
