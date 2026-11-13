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

# Parse checksum sidecars with Python instead of sha256sum -c. MSYS2
# may translate LF sidecars to CRLF, which sha256sum treats as part of the
# filename on Windows. split() normalizes both formats without weakening the
# digest or filename checks.
python3 - "$dist/$archive_name" "$dist/$checksum_name" \
  "$dist/$registry_archive_name" "$dist/$registry_checksum_name" <<'PY'
import hashlib
import sys
from pathlib import Path

for archive_arg, sidecar_arg in zip(sys.argv[1::2], sys.argv[2::2]):
    archive = Path(archive_arg)
    sidecar = Path(sidecar_arg)
    fields = sidecar.read_text(encoding="ascii").split()
    if len(fields) < 2:
        raise SystemExit(f"{sidecar}: malformed checksum sidecar")
    expected_digest = fields[0].lower()
    expected_name = fields[1].lstrip("*")
    if expected_name != archive.name:
        raise SystemExit(
            f"{sidecar}: checksum filename {expected_name!r} != {archive.name!r}"
        )
    digest = hashlib.sha256()
    with archive.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    actual_digest = digest.hexdigest()
    if actual_digest != expected_digest:
        raise SystemExit(
            f"{archive}: SHA-256 mismatch: expected {expected_digest}, got {actual_digest}"
        )
    print(f"{archive.name}: OK")
PY

printf 'Release output verified: %s, %s and matching checksums\n' \
  "$archive_name" "$registry_archive_name"
