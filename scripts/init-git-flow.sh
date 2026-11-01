#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

command -v git >/dev/null 2>&1 || { echo "git is required" >&2; exit 2; }
command -v gix >/dev/null 2>&1 || {
  echo "gix is required. Install it from https://github.com/m-mdy-m/gix, then rerun this script." >&2
  exit 2
}
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
  echo "initialize and make the first Git commit before running this script" >&2
  exit 2
}
git rev-parse --verify HEAD >/dev/null 2>&1 || {
  echo "the repository needs an initial commit before gix flow init can run" >&2
  exit 2
}

if [[ -f .gix/config ]]; then
  echo "gix flow config already exists; checking current flow"
  gix doctor
  exit $?
fi

base_branch="$(git symbolic-ref --quiet --short HEAD || true)"
case "$base_branch" in
  main|master|trunk) ;;
  *)
    echo "run this initializer from main, master, or trunk (current: ${base_branch:-detached HEAD})" >&2
    exit 2
    ;;
esac

cp .gix/config.default .gix/config
if [[ "$base_branch" != main ]]; then
  awk -v branch="$base_branch" '  
    /^  main:$/ { in_main = 1; print; next }
    in_main && /^    name: main$/ { print "    name: " branch; in_main = 0; found = 1; next }
    { print }
    END { if (!found) exit 1 }
  ' .gix/config > .gix/config.tmp || {
    rm -f .gix/config.tmp
    echo "could not update the main branch mapping in .gix/config" >&2
    exit 1
  }
  mv .gix/config.tmp .gix/config
fi
gix flow init

echo
echo "gix flow is ready. Start work with: gix feature start <name>"
