#!/usr/bin/env python3
"""Create a flat, link-free archive from a Kernel install prefix for Registry.

The ordinary GitHub download package keeps its historical top-level directory.
This companion archive starts at the actual install prefix so Registry can
extract include/, lib/, bin/ and share/ directly into cmake-prefix-v1.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import tarfile
import zipfile


class PackageError(ValueError):
    """Raised when an install prefix cannot be represented portably."""


def _safe_relative(path: Path, root: Path) -> str:
    value = path.relative_to(root).as_posix()
    pure = PurePosixPath(value)
    if (
        not value
        or value.startswith("/")
        or "\\" in value
        or any(part in ("", ".", "..") for part in pure.parts)
        or ":" in value
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
    ):
        raise PackageError(f"unsafe package path: {value!r}")
    for part in pure.parts:
        if part.endswith((".", " ")):
            raise PackageError(f"non-portable package path segment: {part!r}")
        stem = part.split(".", 1)[0].upper()
        if stem in {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}:
            raise PackageError(f"reserved Windows device name in package path: {value!r}")
    return value


def _entries(root: Path) -> list[tuple[Path, str]]:
    root = root.expanduser().absolute()
    try:
        root_info = root.lstat()
    except OSError as error:
        raise PackageError(f"cannot inspect package prefix: {root}") from error
    if stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode):
        raise PackageError(f"package prefix is not a real directory: {root}")
    root = root.resolve(strict=True)
    required = (
        "BUILD-INFO.txt",
        "include/cdin/kernel.h",
        "include/m-mdy-m/log.h",
        "lib/libcdin_kernel.a",
        "lib/libm-mdy-m-log.a",
        "lib/pkgconfig/cdin-kernel.pc",
        "lib/pkgconfig/m-mdy-m-log.pc",
    )
    missing = [item for item in required if not (root / item).is_file()]
    if missing:
        raise PackageError("install prefix is incomplete: " + ", ".join(missing))

    result: list[tuple[Path, str]] = []
    seen: set[str] = set()
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = _safe_relative(path, root)
        if path.is_symlink():
            raise PackageError(f"Registry packages cannot contain symlinks: {relative}")
        if not (path.is_file() or path.is_dir()):
            raise PackageError(f"Registry packages cannot contain special entries: {relative}")
        folded = relative.casefold()
        if folded in seen:
            raise PackageError(f"case-folded package path collision: {relative}")
        seen.add(folded)
        result.append((path, relative))
    return result


def _zip_timestamp(epoch: int) -> tuple[int, int, int, int, int, int]:
    # ZIP's minimum timestamp is 1980-01-01 and seconds have 2-second precision.
    import datetime as dt

    value = max(epoch, 315532800)
    stamp = dt.datetime.fromtimestamp(value, tz=dt.timezone.utc)
    return (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second - stamp.second % 2)


def write_prefix_archive(prefix: Path, output: Path, archive_format: str) -> tuple[str, int]:
    prefix = prefix.resolve(strict=True)
    entries = _entries(prefix)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    if temporary.exists():
        temporary.unlink()

    try:
        if archive_format == "tar.gz":
            with tarfile.open(temporary, mode="w:gz", compresslevel=9, format=tarfile.PAX_FORMAT) as archive:
                for path, relative in entries:
                    mode = stat.S_IMODE(path.stat().st_mode) & 0o777
                    if path.is_dir():
                        info = tarfile.TarInfo(relative.rstrip("/") + "/")
                        info.type = tarfile.DIRTYPE
                        info.size = 0
                        info.mode = mode or 0o755
                        archive.addfile(info)
                    else:
                        info = tarfile.TarInfo(relative)
                        info.size = path.stat().st_size
                        info.mode = mode or (0o755 if path.stat().st_mode & 0o111 else 0o644)
                        with path.open("rb") as stream:
                            archive.addfile(info, stream)
        elif archive_format == "zip":
            with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
                epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "315532800"))
                for path, relative in entries:
                    mode = stat.S_IMODE(path.stat().st_mode) & 0o777
                    info = zipfile.ZipInfo(
                        relative.rstrip("/") + ("/" if path.is_dir() else ""),
                        date_time=_zip_timestamp(epoch),
                    )
                    info.create_system = 3
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info.external_attr = (
                        ((stat.S_IFDIR | (mode or 0o755)) << 16) | 0x10
                        if path.is_dir()
                        else (stat.S_IFREG | (mode or 0o644)) << 16
                    )
                    if path.is_dir():
                        archive.writestr(info, b"")
                    else:
                        archive.writestr(info, path.read_bytes())
        else:
            raise PackageError(f"unsupported Registry package archive format: {archive_format}")
        temporary.replace(output)
    except Exception:
        if temporary.exists():
            temporary.unlink()
        raise

    digest = hashlib.sha256()
    with output.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    checksum = digest.hexdigest()
    sidecar = output.with_name(output.name + ".sha256")
    sidecar.write_text(f"{checksum}  {output.name}\n", encoding="ascii")
    return checksum, output.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--format", choices=("tar.gz", "zip"), required=True)
    args = parser.parse_args()
    digest, size = write_prefix_archive(args.prefix, args.output, args.format)
    print(f"Registry install-prefix package: {args.output} ({size} bytes, sha256={digest})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, PackageError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"package-registry-prefix: {error}", file=sys.stderr)
        raise SystemExit(1)
