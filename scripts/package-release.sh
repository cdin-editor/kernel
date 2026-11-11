#!/usr/bin/env bash
set -euo pipefail

: "${PACKAGE_VERSION:?set PACKAGE_VERSION to the package version}"
: "${STAGED_PREFIX:?set STAGED_PREFIX to the DESTDIR/PREFIX installation directory}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"
stage="$(cd "$STAGED_PREFIX" && pwd -P)"
host="$(uname -s 2>/dev/null || printf unknown)"
arch="$(uname -m 2>/dev/null || printf unknown)"

case "$host" in
  Linux*)
    platform=linux
    target_os=linux
    target_abi=gnu-glibc-2.39
    glibc_version="$(getconf GNU_LIBC_VERSION | awk '{print $2}')"
    [[ "$glibc_version" == 2.39 ]] || {
      echo "Kernel Linux packages require glibc 2.39; found $glibc_version." >&2
      exit 1
    }
    ;;
  Darwin)
    platform=macos
    target_os=darwin
    target_abi=macos-15
    macos_major="$(sw_vers -productVersion | cut -d. -f1)"
    [[ "$macos_major" == 15 ]] || {
      echo "Kernel macOS packages require macOS 15; found $macos_major." >&2
      exit 1
    }
    ;;
  MINGW*|MSYS*|CYGWIN*)
    platform=windows
    target_os=windows
    target_abi=windows-ucrt64
    [[ "${MSYSTEM:-}" == UCRT64 ]] || {
      echo "Kernel Windows packages require the MSYS2 UCRT64 environment." >&2
      exit 1
    }
    ;;
  *)
    echo "unsupported packaging host: $host" >&2
    exit 2
    ;;
esac
case "$arch" in
  amd64|x64|AMD64) arch=x86_64 ;;
  aarch64|arm64) arch=arm64 ;;
  *)
    echo "unsupported packaging architecture: $arch" >&2
    exit 2
    ;;
esac

asset_base="cdin-kernel-${PACKAGE_VERSION}-${platform}-${arch}"
# dist/ is a public artifact directory, not a package workspace. Keeping the
# expanded tree elsewhere prevents broad artifact globs from publishing files.
dist="$repo_root/dist"
package_name="$asset_base"
package_parent="$repo_root/build/package"
package_root="$package_parent/$package_name"
mkdir -p "$package_parent" "$dist"
rm -rf "$package_root"
mkdir -p "$package_root/share/cdin-kernel"

for required in include lib; do
  [[ -d "$stage/$required" ]] || {
    echo "staged install prefix is missing $required/" >&2
    exit 1
  }
done
cp -R "$stage/include" "$package_root/"
cp -R "$stage/lib" "$package_root/"
if [[ -d "$stage/bin" ]]; then cp -R "$stage/bin" "$package_root/"; fi
if [[ -d "$stage/share" ]]; then cp -R "$stage/share" "$package_root/"; fi
cp "$repo_root/README.md" "$repo_root/CHANGELOG.md" "$repo_root/LICENSE" "$package_root/"
cp "$repo_root/packaging/release-install.sh" "$package_root/install.sh"
cp "$repo_root/packaging/release-install.ps1" "$package_root/install.ps1"
cp "$repo_root/packaging/RELEASE-INSTALL.md" "$package_root/INSTALL.md"
chmod 0755 "$package_root/install.sh"
cp -R "$repo_root/docs" "$package_root/"
cp -R "$repo_root/examples" "$package_root/"
cp -R "$repo_root/docs" "$package_root/share/cdin-kernel/"
mkdir -p "$package_root/share/cdin-kernel/examples"
cp "$repo_root/examples/c/quickstart.c" "$package_root/share/cdin-kernel/examples/"

# Registry's portable archive contract rejects symlinks and special entries.
# Materialize shared-library aliases as regular files before creating the archive.
python scripts/normalize-package-links.py "$package_root"

# The ABI label is independent from the Kernel package version.
# deps.lock is the authoritative logger SDK pin used by this build.
# shellcheck source=../deps.lock
source "$repo_root/deps.lock"
kernel_abi="$({
  sed -n 's/^#define CDIN_KERNEL_ABI_MAJOR \([0-9][0-9]*\)u/\1/p' "$repo_root/include/cdin/kernel.h"
  sed -n 's/^#define CDIN_KERNEL_ABI_MINOR \([0-9][0-9]*\)u/\1/p' "$repo_root/include/cdin/kernel.h"
  sed -n 's/^#define CDIN_KERNEL_ABI_PATCH \([0-9][0-9]*\)u/\1/p' "$repo_root/include/cdin/kernel.h"
} | paste -sd . -)"
[[ "$kernel_abi" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "could not resolve the public Kernel ABI version from kernel.h" >&2
  exit 1
}
runner="github-actions-${RUNNER_OS:-local}-${RUNNER_ARCH:-$arch}"
compiler_version="$(cc --version | head -n 1)"
cmake_version="$(cmake --version | head -n 1)"
{
  printf 'Package version: %s\n' "$PACKAGE_VERSION"
  printf 'C ABI version: %s\n' "$kernel_abi"
  printf 'Target OS: %s\n' "$target_os"
  printf 'Target architecture: %s\n' "$arch"
  printf 'Target ABI: %s\n' "$target_abi"
  printf 'Runner: %s\n' "$runner"
  printf 'Compiler: %s\n' "$compiler_version"
  printf 'CMake: %s\n' "$cmake_version"
  printf 'm-mdy-m/log.c: %s\n' "$M_MDY_M_LOG_VERSION"
} > "$package_root/BUILD-INFO.txt"

if [[ "$platform" == windows ]]; then
  command -v zip >/dev/null || { echo 'zip is required on Windows packaging runner' >&2; exit 2; }
  (cd "$package_parent" && zip -qr "$dist/${asset_base}.zip" "$package_name")
  asset="$dist/${asset_base}.zip"
else
  tar -C "$package_parent" -czf "$dist/${asset_base}.tar.gz" "$package_name"
  asset="$dist/${asset_base}.tar.gz"
fi

if command -v sha256sum >/dev/null 2>&1; then
  (cd "$dist" && sha256sum "$(basename "$asset")" > "$(basename "$asset").sha256")
else
  (cd "$dist" && shasum -a 256 "$(basename "$asset")" > "$(basename "$asset").sha256")
fi
printf 'Created %s and its SHA-256 checksum.\n' "$asset"

# Publish a second archive whose contents start at include/, lib/, bin/ and
# share/. Registry extracts this directly into its cmake-prefix-v1 package path.
if [[ "$platform" == windows ]]; then
  registry_asset="$dist/${asset_base}-cmake-prefix.zip"
  registry_format=zip
else
  registry_asset="$dist/${asset_base}-cmake-prefix.tar.gz"
  registry_format=tar.gz
fi
python scripts/package_registry_prefix.py \
  --prefix "$package_root" \
  --output "$registry_asset" \
  --format "$registry_format"
