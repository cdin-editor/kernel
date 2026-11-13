#!/usr/bin/env python3
"""Generate immutable Kernel Component Release Metadata from five verified packages."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tarfile
import unicodedata
import zipfile
from urllib.parse import quote

REPOSITORY = "https://github.com/cdin-editor/kernel"
GIT_REVISION = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-((?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*))*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)

TARGETS = (
    {
        "key": "linux-x86_64",
        "os": "linux",
        "architecture": "x86_64",
        "abi": "gnu-glibc-2.39",
        "platform": "linux",
        "extension": ".tar.gz",
    },
    {
        "key": "linux-arm64",
        "os": "linux",
        "architecture": "arm64",
        "abi": "gnu-glibc-2.39",
        "platform": "linux",
        "extension": ".tar.gz",
    },
    {
        "key": "darwin-x86_64",
        "os": "darwin",
        "architecture": "x86_64",
        "abi": "macos-15",
        "platform": "macos",
        "extension": ".tar.gz",
    },
    {
        "key": "darwin-arm64",
        "os": "darwin",
        "architecture": "arm64",
        "abi": "macos-15",
        "platform": "macos",
        "extension": ".tar.gz",
    },
    {
        "key": "windows-x86_64",
        "os": "windows",
        "architecture": "x86_64",
        "abi": "windows-ucrt64",
        "platform": "windows",
        "extension": ".zip",
    },
)


class MetadataError(ValueError):
    """Raised when the release package inventory is incomplete or unsafe."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_archive_name(raw: str) -> str:
    if not raw or "\x00" in raw or "\\" in raw or raw.startswith("/"):
        raise MetadataError(f"unsafe archive path: {raw!r}")
    name = raw[:-1] if raw.endswith("/") else raw
    if not name:
        raise MetadataError(f"empty archive path: {raw!r}")
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise MetadataError(f"non-canonical archive path: {raw!r}")
    if any(ord(char) < 32 or ord(char) == 127 for char in name) or ":" in name:
        raise MetadataError(f"non-portable archive path: {raw!r}")
    for part in parts:
        if part.endswith((".", " ")):
            raise MetadataError(f"non-portable archive path segment: {part!r}")
        stem = part.split(".", 1)[0].upper()
        if stem in {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}:
            raise MetadataError(f"reserved Windows device name in archive path: {raw!r}")
    return name


def _validate_archive_paths(names: list[str]) -> None:
    seen: dict[str, tuple[str, bool]] = {}
    kinds: dict[str, bool] = {}
    for raw in names:
        name = _safe_archive_name(raw)
        is_dir = raw.endswith("/")
        normalized = unicodedata.normalize("NFC", name)
        folded = normalized.casefold()
        previous = seen.get(folded)
        if previous is not None:
            raise MetadataError(f"duplicate or case/Unicode-colliding package path: {raw!r}")
        seen[folded] = (normalized, is_dir)
        parts = folded.split("/")
        for index in range(1, len(parts)):
            parent = "/".join(parts[:index])
            if kinds.get(parent) is False:
                raise MetadataError(f"file/directory path collision in package: {raw!r}")
        if folded in kinds and (kinds[folded] is False or not is_dir):
            raise MetadataError(f"file/directory path collision in package: {raw!r}")
        kinds[folded] = is_dir


def _read_tar_build_info(path: Path) -> str:
    try:
        with tarfile.open(path, mode="r:gz") as archive:
            members = archive.getmembers()
            names = [member.name for member in members]
            _validate_archive_paths(names)
            for member in members:
                if not (member.isfile() or member.isdir()):
                    raise MetadataError(f"links and special entries are forbidden: {member.name}")
                if member.mode & 0o7000:
                    raise MetadataError(f"privileged package mode is forbidden: {member.name}")
            paths = {_safe_archive_name(name) for name in names}
            required = {
                "BUILD-INFO.txt",
                "include/cdin/kernel.h",
                "include/m-mdy-m/log.h",
                "lib/libcdin_kernel.a",
                "lib/libm-mdy-m-log.a",
                "lib/pkgconfig/cdin-kernel.pc",
                "lib/pkgconfig/m-mdy-m-log.pc",
            }
            missing = sorted(required - paths)
            if missing:
                raise MetadataError(
                    "cmake-prefix-v1 archive must expose the install prefix at its root; "
                    "missing: " + ", ".join(missing)
                )
            matches = [member for member in members if member.name == "BUILD-INFO.txt" and member.isfile()]
            if len(matches) != 1:
                raise MetadataError("cmake-prefix-v1 archive must contain exactly one root BUILD-INFO.txt")
            stream = archive.extractfile(matches[0])
            if stream is None:
                raise MetadataError("cannot read package BUILD-INFO.txt")
            data = stream.read(64 * 1024 + 1)
            if len(data) > 64 * 1024:
                raise MetadataError("package BUILD-INFO.txt exceeds 64 KiB")
            return data.decode("utf-8")
    except (OSError, tarfile.TarError, UnicodeDecodeError) as error:
        raise MetadataError(f"invalid TAR.GZ package {path.name}: {error}") from error


def _read_zip_build_info(path: Path) -> str:
    try:
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            names = [item.filename for item in infos]
            _validate_archive_paths(names)
            for item in infos:
                mode = item.external_attr >> 16
                file_type = stat.S_IFMT(mode)
                if file_type == stat.S_IFLNK or (file_type not in (0, stat.S_IFREG, stat.S_IFDIR)):
                    raise MetadataError(f"links and special entries are forbidden: {item.filename}")
                if mode & 0o7000:
                    raise MetadataError(f"privileged package mode is forbidden: {item.filename}")
                if item.is_dir() and item.file_size != 0:
                    raise MetadataError(f"package directory entry is not empty: {item.filename}")
            paths = {_safe_archive_name(name) for name in names}
            required = {
                "BUILD-INFO.txt",
                "include/cdin/kernel.h",
                "include/m-mdy-m/log.h",
                "lib/libcdin_kernel.a",
                "lib/libm-mdy-m-log.a",
                "lib/pkgconfig/cdin-kernel.pc",
                "lib/pkgconfig/m-mdy-m-log.pc",
            }
            missing = sorted(required - paths)
            if missing:
                raise MetadataError(
                    "cmake-prefix-v1 archive must expose the install prefix at its root; "
                    "missing: " + ", ".join(missing)
                )
            matches = [item for item in infos if item.filename == "BUILD-INFO.txt" and not item.is_dir()]
            if len(matches) != 1:
                raise MetadataError("cmake-prefix-v1 archive must contain exactly one root BUILD-INFO.txt")
            data = archive.read(matches[0])
            if len(data) > 64 * 1024:
                raise MetadataError("package BUILD-INFO.txt exceeds 64 KiB")
            return data.decode("utf-8")
    except (OSError, zipfile.BadZipFile, RuntimeError, UnicodeDecodeError) as error:
        raise MetadataError(f"invalid ZIP package {path.name}: {error}") from error


def _parse_build_info(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            key = key.strip()
            value = value.strip()
            if key in fields:
                raise MetadataError(f"duplicate package BUILD-INFO field: {key}")
            fields[key] = value
    required = (
        "Package version",
        "C ABI version",
        "Target OS",
        "Target architecture",
        "Target ABI",
        "Runner",
        "Compiler",
        "CMake",
        "m-mdy-m/log.c",
    )
    missing = [key for key in required if not fields.get(key)]
    if missing:
        raise MetadataError("package BUILD-INFO is missing fields: " + ", ".join(missing))
    return fields


def _find_one(root: Path, filename: str) -> Path:
    matches = []
    for candidate in root.rglob(filename):
        if candidate.is_symlink():
            raise MetadataError(f"release input must be a regular file, not a symlink: {candidate}")
        if candidate.is_file():
            matches.append(candidate)
    if len(matches) != 1:
        raise MetadataError(f"expected exactly one {filename}; found {len(matches)}")
    path = matches[0].resolve(strict=True)
    try:
        path.relative_to(root.resolve(strict=True))
    except ValueError as error:
        raise MetadataError(f"release input escapes artifact directory: {filename}") from error
    return path


def _expected_filename(version: str, target: dict[str, str]) -> str:
    architecture = target["architecture"]
    return (
        f"cdin-kernel-{version}-{target['platform']}-{architecture}"
        f"-cmake-prefix{target['extension']}"
    )


def build_metadata(
    *,
    tag: str,
    source_revision: str,
    artifact_dir: Path,
    kernel_abi: str,
    repository_uri: str = REPOSITORY,
) -> dict[str, object]:
    if repository_uri != REPOSITORY:
        raise MetadataError(f"unexpected publisher repository: {repository_uri}")
    if not tag.startswith("v") or not SEMVER.fullmatch(tag[1:]):
        raise MetadataError("release tag must be v-prefixed Semantic Versioning")
    version = tag[1:]
    if not GIT_REVISION.fullmatch(source_revision):
        raise MetadataError("source revision must be a lowercase full Git commit ID")
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", kernel_abi):
        raise MetadataError("kernel ABI must be a three-part numeric version")
    if not artifact_dir.is_dir():
        raise MetadataError(f"artifact directory does not exist: {artifact_dir}")
    artifact_dir = artifact_dir.resolve(strict=True)

    artifacts: list[dict[str, object]] = []
    expected_filenames = {_expected_filename(version, target) for target in TARGETS}
    found_archive_files = [
        path for path in artifact_dir.rglob(f"cdin-kernel-{version}-*-cmake-prefix.*")
        if path.is_file() and path.name.endswith((".tar.gz", ".zip"))
    ]
    actual_filenames = {path.name for path in found_archive_files}
    if actual_filenames != expected_filenames or len(found_archive_files) != len(TARGETS):
        missing = sorted(expected_filenames - actual_filenames)
        unexpected = sorted(actual_filenames - expected_filenames)
        raise MetadataError(
            f"Registry package inventory must contain exactly five cmake-prefix archives; "
            f"missing={missing}, unexpected={unexpected}"
        )
    expected_sidecars = {name + ".sha256" for name in expected_filenames}
    actual_sidecars = {
        path.name for path in artifact_dir.rglob("cdin-kernel-*.sha256")
        if path.is_file() and "-cmake-prefix." in path.name
    }
    if actual_sidecars != expected_sidecars:
        missing = sorted(expected_sidecars - actual_sidecars)
        unexpected = sorted(actual_sidecars - expected_sidecars)
        raise MetadataError(
            f"Registry package checksum inventory mismatch; missing={missing}, unexpected={unexpected}"
        )

    encoded_tag = quote(tag, safe=".-")
    for target in TARGETS:
        filename = _expected_filename(version, target)
        archive_path = _find_one(artifact_dir, filename)
        checksum_path = _find_one(archive_path.parent, filename + ".sha256")
        digest = sha256_file(archive_path)
        size = archive_path.stat().st_size
        if size <= 0:
            raise MetadataError(f"package archive is empty: {filename}")

        checksum_fields = checksum_path.read_text(encoding="ascii").split()
        if len(checksum_fields) != 2 or checksum_fields[0].lower() != digest:
            raise MetadataError(f"SHA-256 sidecar does not match package bytes: {filename}")
        if checksum_fields[1].lstrip("*") != filename:
            raise MetadataError(f"SHA-256 sidecar names a different package: {filename}")

        build_info_text = (
            _read_zip_build_info(archive_path)
            if target["extension"] == ".zip"
            else _read_tar_build_info(archive_path)
        )
        build_info = _parse_build_info(build_info_text)
        expected_fields = {
            "Package version": version,
            "C ABI version": kernel_abi,
            "Target OS": target["os"],
            "Target architecture": target["architecture"],
            "Target ABI": target["abi"],
        }
        for key, expected in expected_fields.items():
            if build_info[key] != expected:
                raise MetadataError(
                    f"{filename}: BUILD-INFO {key!r} is {build_info[key]!r}, expected {expected!r}"
                )

        platform_files = {
            "windows": ("lib/libcdin_kernel.dll.a", "bin/cdin_kernel.dll"),
            "darwin": (f"lib/libcdin_kernel.{kernel_abi}.dylib",),
            "linux": (f"lib/libcdin_kernel.so.{kernel_abi}",),
        }[target["os"]]
        if target["extension"] == ".zip":
            with zipfile.ZipFile(archive_path) as package:
                member_names = {item.filename.rstrip("/") for item in package.infolist()}
        else:
            with tarfile.open(archive_path, mode="r:gz") as package:
                member_names = {item.name.rstrip("/") for item in package.getmembers()}
        missing_platform_files = sorted(set(platform_files) - member_names)
        if missing_platform_files:
            raise MetadataError(
                f"{filename}: cmake-prefix-v1 package is missing target libraries: "
                + ", ".join(missing_platform_files)
            )

        artifact_id = (
            f"kernel-{target['os']}-{target['architecture'].replace('_', '-')}-{target['abi']}"
        )
        artifacts.append(
            {
                "artifact_id": artifact_id,
                "kind": "native-development-package",
                "target": {
                    "os": target["os"],
                    "architecture": target["architecture"],
                    "abi": target["abi"],
                },
                "format": "zip" if target["extension"] == ".zip" else "tar.gz",
                "file_name": filename,
                "uri": f"{REPOSITORY}/releases/download/{encoded_tag}/{filename}",
                "sha256": digest,
                "size_bytes": size,
                "layout": "cmake-prefix-v1",
                "source_unit_ids": ["kernel-source"],
                "build": {
                    "recipe": {
                        "source_unit_id": "kernel-source",
                        "path": "scripts/ci-package.sh",
                    },
                    "toolchain": [
                        {"name": "compiler", "version": build_info["Compiler"]},
                        {"name": "CMake", "version": build_info["CMake"]},
                        {"name": "m-mdy-m/log.c", "version": build_info["m-mdy-m/log.c"]},
                    ],
                    "runner": build_info["Runner"],
                },
            }
        )

    return {
        "schema_version": "1.2.0",
        "component_id": "kernel",
        "variant": "default",
        "version": version,
        "status": "active",
        "source_units": [
            {
                "source_unit_id": "kernel-source",
                "repository_uri": REPOSITORY,
                "revision": source_revision,
            }
        ],
        "publication": {
            "provider": "github-releases-v1",
            "repository_uri": REPOSITORY,
            "tag": tag,
            "tag_commit": source_revision,
            "metadata_asset_name": f"kernel-{version}.json",
        },
        "artifacts": artifacts,
        "dependencies": [],
        "contracts": {
            "provides": [{"contract_id": "org.cdin.kernel-abi", "version": kernel_abi}],
            "requires": [],
        },
    }


def read_kernel_abi(header: Path) -> str:
    content = header.read_text(encoding="utf-8")
    values: dict[str, str] = {}
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(
            rf"^\s*#define\s+CDIN_KERNEL_ABI_{name}\s+([0-9]+)u?\s*$",
            content,
            flags=re.MULTILINE,
        )
        if not match:
            raise MetadataError(f"cannot find CDIN_KERNEL_ABI_{name} in {header}")
        values[name] = match.group(1)
    return ".".join(values[name] for name in ("MAJOR", "MINOR", "PATCH"))


def resolve_source_revision(*, tag: str, source_revision: str, dry_run: bool) -> str:
    """Bind metadata provenance to HEAD, requiring a real tag only for releases."""
    try:
        head_revision = subprocess.check_output(
            ["git", "rev-parse", "--verify", "HEAD"],
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
    except subprocess.CalledProcessError as error:
        raise MetadataError(f"cannot resolve checked-out HEAD: {error}") from error

    if not GIT_REVISION.fullmatch(head_revision):
        raise MetadataError("checked-out HEAD must resolve to a full lowercase Git commit ID")

    if dry_run:
        if source_revision != head_revision:
            raise MetadataError(
                f"dry-run source mismatch: HEAD={head_revision}, Pipeline source={source_revision}"
            )
        return head_revision

    try:
        tag_revision = subprocess.check_output(
            ["git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"],
            text=True,
            stderr=subprocess.PIPE,
        ).strip()
    except subprocess.CalledProcessError as error:
        raise MetadataError(f"cannot resolve release tag {tag!r}: {error}") from error

    if not GIT_REVISION.fullmatch(tag_revision):
        raise MetadataError("release tag must resolve to a full lowercase Git commit ID")
    if tag_revision != source_revision or head_revision != tag_revision:
        raise MetadataError(
            f"release source mismatch: tag={tag_revision}, HEAD={head_revision}, "
            f"Pipeline source={source_revision}"
        )
    return tag_revision


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default=os.environ.get("PIPELINE_RELEASE_TAG", ""))
    parser.add_argument("--source-revision", default=os.environ.get("PIPELINE_SOURCE_SHA", ""))
    parser.add_argument("--artifact-dir", type=Path, default=Path("release-assets"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--header", type=Path, default=Path("include/cdin/kernel.h"))
    args = parser.parse_args()

    if not args.tag or not args.source_revision:
        parser.error("--tag and --source-revision are required (or set Pipeline release variables)")
    dry_run = (
        os.environ.get("PIPELINE_DRY_RUN", "false").strip().lower() == "true"
        and os.environ.get("GITHUB_REF_TYPE", "branch") != "tag"
    )
    resolved_revision = resolve_source_revision(
        tag=args.tag,
        source_revision=args.source_revision,
        dry_run=dry_run,
    )


    metadata = build_metadata(
        tag=args.tag,
        source_revision=resolved_revision,
        artifact_dir=args.artifact_dir,
        kernel_abi=read_kernel_abi(args.header),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json.dumps(metadata, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")
    temporary = args.output.with_name(args.output.name + ".tmp")
    temporary.write_bytes(encoded)
    temporary.replace(args.output)
    print(
        f"Generated {args.output}: Kernel {metadata['version']}, "
        f"ABI {metadata['contracts']['provides'][0]['version']}, "
        f"{len(metadata['artifacts'])} verified target artifacts."
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (MetadataError, OSError, subprocess.CalledProcessError) as error:
        print(f"generate-registry-metadata: {error}", file=sys.stderr)
        raise SystemExit(1)
