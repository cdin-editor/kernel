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
mkdir -p "$prefix/bin"

case "$host" in
  MINGW*|MSYS*|CYGWIN*)
    static_exe="$prefix/bin/.cdin-smoke-static.exe"
    shared_exe="$prefix/bin/.cdin-smoke-shared.exe"
    trap 'rm -f "$static_exe" "$shared_exe"' EXIT
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
      -I"$prefix/include" "$source_file" "$prefix/lib/libcdin_kernel.a" \
      -pthread -o "$static_exe"
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
      -I"$prefix/include" "$source_file" "$prefix/lib/libcdin_kernel.dll.a" \
      -o "$shared_exe"
    ;;
  *)
    static_exe="$prefix/bin/.cdin-smoke-static"
    shared_exe="$prefix/bin/.cdin-smoke-shared"
    trap 'rm -f "$static_exe" "$shared_exe"' EXIT
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
      -I"$prefix/include" "$source_file" "$prefix/lib/libcdin_kernel.a" \
      -pthread -o "$static_exe"
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
  PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    pkg-config --validate cdin-kernel
  version="$(PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" pkg-config --modversion cdin-kernel)"
  if [[ -z "$version" ]]; then
    echo "pkg-config metadata is missing a version" >&2
    exit 1
  fi
  echo "pkg-config metadata: $version"
fi

echo "installed static and shared consumers: PASS ($host)"
