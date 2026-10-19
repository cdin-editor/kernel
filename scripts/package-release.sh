#!/usr/bin/env bash
set -euo pipefail

: "${PACKAGE_VERSION:?set PACKAGE_VERSION to the package version}"
: "${STAGED_PREFIX:?set STAGED_PREFIX to the DESTDIR/PREFIX installation directory}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
stage="$(cd "$STAGED_PREFIX" && pwd -P)"
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
dist="$repo_root/dist"
package_name="$asset_base"
package_root="$dist/$package_name"
mkdir -p "$dist"
rm -rf "$package_root"
mkdir -p "$package_root/share/cdin-kernel"

cp -R "$stage/include" "$package_root/"
cp -R "$stage/lib" "$package_root/"
if [[ -d "$stage/bin" ]]; then cp -R "$stage/bin" "$package_root/"; fi
cp "$repo_root/README.md" "$repo_root/CHANGELOG.md" "$repo_root/LICENSE" "$package_root/"
cp "$repo_root/packaging/release-install.sh" "$package_root/install.sh"
cp "$repo_root/packaging/release-install.ps1" "$package_root/install.ps1"
cp "$repo_root/packaging/RELEASE-INSTALL.md" "$package_root/INSTALL.md"
chmod 0755 "$package_root/install.sh"
cp -R "$repo_root/docs" "$package_root/share/cdin-kernel/"
mkdir -p "$package_root/share/cdin-kernel/examples"
cp "$repo_root/examples/c/quickstart.c" "$package_root/share/cdin-kernel/examples/"
printf 'Package version: %s\nC ABI version: %s\nPlatform: %s\nArchitecture: %s\n' \
  "$PACKAGE_VERSION" "$({ sed -n 's/^#define CDIN_KERNEL_ABI_MAJOR \([0-9][0-9]*\)u/\1/p' "$repo_root/include/cdin/kernel.h"; sed -n 's/^#define CDIN_KERNEL_ABI_MINOR \([0-9][0-9]*\)u/\1/p' "$repo_root/include/cdin/kernel.h"; sed -n 's/^#define CDIN_KERNEL_ABI_PATCH \([0-9][0-9]*\)u/\1/p' "$repo_root/include/cdin/kernel.h"; } | paste -sd . -)" \
  "$platform" "$arch" > "$package_root/BUILD-INFO.txt"

if [[ "$platform" == windows ]]; then
  command -v zip >/dev/null || { echo 'zip is required on Windows packaging runner' >&2; exit 2; }
  (cd "$dist" && zip -qr "${asset_base}.zip" "$package_name")
  asset="$dist/${asset_base}.zip"
else
  tar -C "$dist" -czf "$dist/${asset_base}.tar.gz" "$package_name"
  asset="$dist/${asset_base}.tar.gz"
fi

if command -v sha256sum >/dev/null 2>&1; then
  (cd "$dist" && sha256sum "$(basename "$asset")" > "$(basename "$asset").sha256")
else
  (cd "$dist" && shasum -a 256 "$(basename "$asset")" > "$(basename "$asset").sha256")
fi
printf 'Created %s and its SHA-256 checksum.\n' "$asset"
