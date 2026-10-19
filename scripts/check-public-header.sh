#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_dir="$repo_root/build/header-check"
cc="${CC:-cc}"
cxx="${CXX:-c++}"
mkdir -p "$build_dir"
trap 'rm -rf "$build_dir"' EXIT

printf '#include "cdin/kernel.h"\nint main(void) { return CDIN_KERNEL_ABI_MAJOR > 999u; }\n' |
  "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror -I"$repo_root/include" \
    -x c -c -o "$build_dir/header-c.o" -
printf '#include "cdin/kernel.h"\nint main() { return CDIN_KERNEL_ABI_MAJOR > 999u; }\n' |
  "$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -I"$repo_root/include" \
    -x c++ -c -o "$build_dir/header-cxx.o" -

echo 'public header C11/C++17 compile: PASS'
