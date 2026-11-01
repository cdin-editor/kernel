#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
# shellcheck source=../deps.lock
source "$repo_root/deps.lock"

prefix="$repo_root/.deps"
while (($#)); do
  case "$1" in
    --prefix)
      [[ $# -ge 2 && -n "$2" ]] || { echo "--prefix requires a path" >&2; exit 2; }
      prefix="$2"
      shift 2
      ;;
    --help|-h)
      printf 'Usage: %s [--prefix PATH]\n' "$0"
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

for tool in git cmake pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "missing required tool: $tool" >&2
    exit 2
  }
done

source_dir="$repo_root/build/deps/log.c-src"
build_dir="$repo_root/build/deps/log.c-build"
mkdir -p "$(dirname "$source_dir")"

if [[ ! -d "$source_dir/.git" ]]; then
  rm -rf "$source_dir"
  git clone --filter=blob:none --no-checkout "$M_MDY_M_LOG_REPOSITORY" "$source_dir"
fi

git -C "$source_dir" fetch --no-tags --depth 1 origin "$M_MDY_M_LOG_COMMIT"
git -C "$source_dir" checkout --detach --force "$M_MDY_M_LOG_COMMIT"

cmake -S "$source_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_TESTING=OFF \
  -DM_MDY_M_LOG_BUILD_TESTS=OFF \
  -DM_MDY_M_LOG_BUILD_EXAMPLES=OFF
cmake --build "$build_dir" --config Release --parallel
cmake --install "$build_dir" --config Release

printf '\nInstalled m-mdy-m/log.c %s (%s) into %s\n' \
  "$M_MDY_M_LOG_VERSION" "$M_MDY_M_LOG_COMMIT" "$prefix"

pc_file="$prefix/lib/pkgconfig/m-mdy-m-log.pc"
if [[ ! -f "$pc_file" ]]; then
  printf 'error: expected pkg-config file was not installed: %s\n' "$pc_file" >&2
  exit 1
fi

pkgconfig_dir="$(cd "$(dirname "$pc_file")" && pwd -P)"
if command -v cygpath >/dev/null 2>&1; then
  # MSYS2's pkgconf is a native Windows program and needs a Windows path.
  pkgconfig_dir="$(cygpath -m "$pkgconfig_dir")"
fi

if ! installed_version="$(PKG_CONFIG_PATH="$pkgconfig_dir" pkg-config --modversion m-mdy-m-log 2>&1)"; then
  printf 'error: pkg-config cannot resolve m-mdy-m-log from %s\n%s\n' \
    "$pkgconfig_dir" "$installed_version" >&2
  exit 1
fi
if [[ "$installed_version" != "$M_MDY_M_LOG_VERSION" ]]; then
  printf 'error: installed m-mdy-m-log version mismatch: expected %s, found %s\n' \
    "$M_MDY_M_LOG_VERSION" "$installed_version" >&2
  exit 1
fi
printf 'Verified pkg-config module m-mdy-m-log %s from %s\n' \
  "$installed_version" "$pkgconfig_dir"
