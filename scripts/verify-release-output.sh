#!/usr/bin/env bash
set -euo pipefail

: "${PACKAGE_VERSION:?set PACKAGE_VERSION to the package version}"
dist="${1:-dist}"

[[ -d "$dist" ]] || { echo "release output directory does not exist: $dist" >&2; exit 2; }

host="$(uname -s 2>/dev/null || printf unknown)"
arch="$(uname -m 2>/dev/null || printf unknown)"
case "$host" in
  Linux*) platform=linux ;;
  Darwin) platform=macos ;;
  MINGW*|MSYS*|CYGWIN*) platform=windows ;;
  *) echo "unsupported packaging host: $host" >&2; exit 2 ;;
esac
case "$arch" in
  x86_64|amd64|x64|AMD64) arch=x86_64 ;;
  aarch64|arm64) arch=arm64 ;;
  *) echo "unsupported packaging architecture: $arch" >&2; exit 2 ;;
esac

asset_base="cdin-kernel-${PACKAGE_VERSION}-${platform}-${arch}"
if [[ "$platform" == windows ]]; then
  archive_name="${asset_base}.zip"
  registry_archive_name="${asset_base}-cmake-prefix.zip"
else
  archive_name="${asset_base}.tar.gz"
  registry_archive_name="${asset_base}-cmake-prefix.tar.gz"
fi
checksum_name="${archive_name}.sha256"
registry_checksum_name="${registry_archive_name}.sha256"
expected=("$archive_name" "$checksum_name" "$registry_archive_name" "$registry_checksum_name")

# Each target contributes the existing user archive and a separate flat
# cmake-prefix-v1 Registry package, each with its own verified checksum.
shopt -s nullglob
entries=("$dist"/*)
if [[ ${#entries[@]} -ne ${#expected[@]} ]]; then
  printf 'expected exactly %s release output files in %s, found %s\n' \
    "${#expected[@]}" "$dist" "${#entries[@]}" >&2
  printf '  %s\n' "${entries[@]}" >&2
  exit 1
fi

for name in "${expected[@]}"; do
  path="$dist/$name"
  [[ -f "$path" && ! -L "$path" ]] || {
    echo "missing or unsafe release output: $path" >&2
    exit 1
  }
done

for entry in "${entries[@]}"; do
  if [[ ! -f "$entry" || -L "$entry" ]]; then
    echo "release output must contain regular files only: $entry" >&2
    exit 1
  fi
  name="$(basename "$entry")"
  found=0
  for expected_name in "${expected[@]}"; do
    [[ "$name" == "$expected_name" ]] && found=1
  done
  [[ "$found" -eq 1 ]] || { echo "unexpected release output: $entry" >&2; exit 1; }
done

if command -v sha256sum >/dev/null 2>&1; then
  (cd "$dist" && sha256sum -c "$checksum_name" && sha256sum -c "$registry_checksum_name")
elif command -v shasum >/dev/null 2>&1; then
  (cd "$dist" && shasum -a 256 -c "$checksum_name" && shasum -a 256 -c "$registry_checksum_name")
else
  echo 'neither sha256sum nor shasum is available to verify release archives' >&2
  exit 2
fi

printf 'Release output verified: %s, %s and matching checksums\n' \
  "$archive_name" "$registry_archive_name"
