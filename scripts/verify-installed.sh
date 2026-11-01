#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <installed-prefix>" >&2
  exit 2
fi

prefix="$(cd "$1" && pwd -P)"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cc="${CC:-cc}"
host="$(uname -s 2>/dev/null || printf unknown)"
source_file="$repo_root/examples/c/quickstart.c"

# pkgconf in MSYS2/UCRT64 is a native Windows executable. Give it a native
# Windows path and isolate the staged package metadata from inherited paths
# (notably the Python setup action's PKG_CONFIG_PATH).
pkg_config_dir="$prefix/lib/pkgconfig"
case "$host" in
  MINGW*|MSYS*|CYGWIN*)
    command -v cygpath >/dev/null 2>&1 || {
      echo "cygpath is required to validate staged pkg-config metadata on Windows" >&2
      exit 2
    }
    pkg_config_dir="$(cygpath -m "$pkg_config_dir")"
    ;;
esac
export PKG_CONFIG_PATH="$pkg_config_dir"

mkdir -p "$prefix/bin"

for required in "$prefix/include/m-mdy-m/log.h" "$prefix/lib/libm-mdy-m-log.a" \
                "$prefix/lib/pkgconfig/m-mdy-m-log.pc" "$prefix/lib/pkgconfig/cdin-kernel.pc"; do
  [[ -f "$required" ]] || { echo "installed dependency artifact missing: $required" >&2; exit 1; }
done

# Fail early with the exact search path if pkg-config cannot resolve the
# staged metadata. Do not append the runner's inherited PKG_CONFIG_PATH: on
# Windows its drive-letter colons are not valid POSIX path-list separators.
if ! static_cflags="$(pkg-config --cflags cdin-kernel 2>&1)"; then
  echo "pkg-config could not resolve cdin-kernel from staged path: $PKG_CONFIG_PATH" >&2
  echo "$static_cflags" >&2
  exit 1
fi
if ! static_logger_libs="$(pkg-config --libs --static m-mdy-m-log 2>&1)"; then
  echo "pkg-config could not resolve m-mdy-m-log from staged path: $PKG_CONFIG_PATH" >&2
  echo "$static_logger_libs" >&2
  exit 1
fi

case "$host" in
  MINGW*|MSYS*|CYGWIN*)
    static_exe="$prefix/bin/.cdin-smoke-static.exe"
    shared_exe="$prefix/bin/.cdin-smoke-shared.exe"
    trap 'rm -f "$static_exe" "$shared_exe"' EXIT
    # Use the archive path directly: pkg-config --static expands private
    # dependencies but does not force -lcdin_kernel to select the .a over .dll.a.
    # Word splitting is intentional for pkg-config's generated flag lists.
    # shellcheck disable=SC2086
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
      $static_cflags "$source_file" "$prefix/lib/libcdin_kernel.a" \
      $static_logger_libs -pthread -o "$static_exe"
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
      -I"$prefix/include" "$source_file" "$prefix/lib/libcdin_kernel.dll.a" \
      -o "$shared_exe"
    ;;
  *)
    static_exe="$prefix/bin/.cdin-smoke-static"
    shared_exe="$prefix/bin/.cdin-smoke-shared"
    trap 'rm -f "$static_exe" "$shared_exe"' EXIT
    # Use the archive path directly so the static smoke test cannot pick the SO.
    # Word splitting is intentional for pkg-config's generated flag lists.
    # shellcheck disable=SC2086
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
      $static_cflags "$source_file" "$prefix/lib/libcdin_kernel.a" \
      $static_logger_libs -pthread -o "$static_exe"
    case "$host" in
      Darwin)
        rpath='@loader_path/../lib'
        ;;
      *)
        rpath='$ORIGIN/../lib'
        ;;
    esac
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
      -I"$prefix/include" "$source_file" -L"$prefix/lib" -lcdin_kernel \
      -Wl,-rpath,"$rpath" -pthread -o "$shared_exe"
    ;;
esac

for exe in "$static_exe" "$shared_exe"; do
  output="$("$exe")"
  printf '%s\n' "$output"
  if [[ "$output" != *"Hello CDIN"* ]]; then
    echo "installed consumer did not produce the expected output: $exe" >&2
    exit 1
  fi
done

if command -v pkg-config >/dev/null 2>&1; then
  pkg-config --validate cdin-kernel
  pkg-config --validate m-mdy-m-log
  if ! pkg-config --static --libs cdin-kernel | grep -q -- '-lm-mdy-m-log'; then
    echo "cdin-kernel.pc does not expose the static logger dependency" >&2
    exit 1
  fi
  version="$(pkg-config --modversion cdin-kernel)"
  if [[ -z "$version" ]]; then
    echo "pkg-config metadata is missing a version" >&2
    exit 1
  fi
  if [[ -n "${EXPECTED_PACKAGE_VERSION:-}" && "$version" != "$EXPECTED_PACKAGE_VERSION" ]]; then
    echo "pkg-config reports $version; expected package version $EXPECTED_PACKAGE_VERSION" >&2
    exit 1
  fi
  echo "pkg-config metadata: $version (path: $PKG_CONFIG_PATH)"
fi

echo "installed static and shared consumers: PASS ($host)"
