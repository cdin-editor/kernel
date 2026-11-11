#!/usr/bin/env python3
"""Replace safe in-package file symlinks with regular files before publication.

Registry's portable package contract deliberately rejects links and special
archive entries. Shared-library aliases therefore carry identical regular-file
bytes in the published CMake prefix.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import sys


def normalize(root: Path) -> int:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError(f"package root is not a directory: {root}")

    converted = 0
    for current, directories, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        for name in list(directories):
            path = current_path / name
            if path.is_symlink():
                raise ValueError(f"directory symlinks are not permitted in packages: {path.relative_to(root)}")

        for name in files:
            path = current_path / name
            if not path.is_symlink():
                if not path.is_file():
                    raise ValueError(f"package contains a non-regular entry: {path.relative_to(root)}")
                continue

            target = path.resolve(strict=True)
            try:
                target.relative_to(root)
            except ValueError as error:
                raise ValueError(
                    f"package symlink escapes package root: {path.relative_to(root)} -> {target}"
                ) from error
            if not target.is_file():
                raise ValueError(f"package symlink does not resolve to a regular file: {path.relative_to(root)}")

            data = target.read_bytes()
            executable = bool(target.stat().st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
            path.unlink()
            path.write_bytes(data)
            path.chmod(0o755 if executable else 0o644)
            converted += 1

    remaining = [path for path in root.rglob("*") if path.is_symlink()]
    if remaining:
        raise ValueError(f"package still contains symlinks: {remaining[0].relative_to(root)}")
    return converted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package_root", type=Path)
    args = parser.parse_args()
    count = normalize(args.package_root)
    print(f"Package link normalization: {count} file alias(es) converted; no links remain.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"normalize-package-links: {error}", file=sys.stderr)
        raise SystemExit(1)
