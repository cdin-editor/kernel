from __future__ import annotations

import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
import zipfile

from scripts.generate_registry_metadata import (
    MetadataError,
    TARGETS,
    build_metadata,
)


class RegistryMetadataTests(unittest.TestCase):
    version = "1.2.3"
    revision = "a" * 40
    abi = "1.1.0"

    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _build_info(self, target: dict[str, str]) -> bytes:
        return (
            f"Package version: {self.version}\n"
            f"C ABI version: {self.abi}\n"
            f"Target OS: {target['os']}\n"
            f"Target architecture: {target['architecture']}\n"
            f"Target ABI: {target['abi']}\n"
            "Runner: github-actions-test-runner\n"
            "Compiler: test-compiler 1.0\n"
            "CMake: cmake version 3.30.0\n"
            "m-mdy-m/log.c: 0.1.0\n"
        ).encode("utf-8")

    def _write_archive(self, target: dict[str, str], *, unsafe_link: bool = False) -> Path:
        filename = (
            f"cdin-kernel-{self.version}-{target['platform']}-"
            f"{target['architecture']}{target['extension']}"
        )
        path = self.root / filename
        root_name = f"cdin-kernel-{self.version}"
        build_info = self._build_info(target)

        if target["extension"] == ".zip":
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr(f"{root_name}/BUILD-INFO.txt", build_info)
                archive.writestr(f"{root_name}/include/cdin/kernel.h", b"header")
                if unsafe_link:
                    info = zipfile.ZipInfo(f"{root_name}/lib/libcdin_kernel.so")
                    info.create_system = 3
                    info.external_attr = (0o120777 << 16)
                    archive.writestr(info, b"../outside")
        else:
            with tarfile.open(path, "w:gz") as archive:
                for name, data in (
                    (f"{root_name}/BUILD-INFO.txt", build_info),
                    (f"{root_name}/include/cdin/kernel.h", b"header"),
                ):
                    item = tarfile.TarInfo(name)
                    item.size = len(data)
                    item.mode = 0o644
                    archive.addfile(item, io.BytesIO(data))
                if unsafe_link:
                    item = tarfile.TarInfo(f"{root_name}/lib/libcdin_kernel.so")
                    item.type = tarfile.SYMTYPE
                    item.linkname = "../outside"
                    archive.addfile(item)

        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        (self.root / (filename + ".sha256")).write_text(
            f"{digest}  {filename}\n", encoding="ascii"
        )
        return path

    def _all_archives(self) -> None:
        for target in TARGETS:
            self._write_archive(target)

    def test_generates_complete_component_release_metadata(self) -> None:
        self._all_archives()
        metadata = build_metadata(
            tag=f"v{self.version}",
            source_revision=self.revision,
            artifact_dir=self.root,
            kernel_abi=self.abi,
        )

        self.assertEqual(metadata["schema_version"], "1.2.0")
        self.assertEqual(metadata["component_id"], "kernel")
        self.assertEqual(metadata["publication"]["tag_commit"], self.revision)
        self.assertEqual(metadata["publication"]["metadata_asset_name"], f"kernel-{self.version}.json")
        self.assertEqual(metadata["contracts"]["provides"][0]["version"], self.abi)
        self.assertEqual(len(metadata["artifacts"]), 5)
        self.assertEqual(
            {(item["target"]["os"], item["target"]["architecture"]) for item in metadata["artifacts"]},
            {(item["os"], item["architecture"]) for item in TARGETS},
        )
        for artifact in metadata["artifacts"]:
            self.assertEqual(artifact["kind"], "native-development-package")
            self.assertEqual(artifact["layout"], "cmake-prefix-v1")
            self.assertEqual(artifact["source_unit_ids"], ["kernel-source"])
            self.assertEqual(artifact["build"]["recipe"]["source_unit_id"], "kernel-source")

    def test_rejects_missing_platform_archive(self) -> None:
        self._all_archives()
        (self.root / f"cdin-kernel-{self.version}-windows-x86_64.zip").unlink()
        (self.root / f"cdin-kernel-{self.version}-windows-x86_64.zip.sha256").unlink()
        with self.assertRaisesRegex(MetadataError, "exactly five native archives"):
            build_metadata(
                tag=f"v{self.version}",
                source_revision=self.revision,
                artifact_dir=self.root,
                kernel_abi=self.abi,
            )

    def test_rejects_checksum_mismatch(self) -> None:
        self._all_archives()
        sidecar = self.root / f"cdin-kernel-{self.version}-linux-x86_64.tar.gz.sha256"
        sidecar.write_text(f"{'0' * 64}  {sidecar.name[:-7]}\n", encoding="ascii")
        with self.assertRaisesRegex(MetadataError, "SHA-256 sidecar"):
            build_metadata(
                tag=f"v{self.version}",
                source_revision=self.revision,
                artifact_dir=self.root,
                kernel_abi=self.abi,
            )

    def test_rejects_archive_links(self) -> None:
        self._all_archives()
        (self.root / f"cdin-kernel-{self.version}-linux-x86_64.tar.gz").unlink()
        (self.root / f"cdin-kernel-{self.version}-linux-x86_64.tar.gz.sha256").unlink()
        self._write_archive(TARGETS[0], unsafe_link=True)
        with self.assertRaisesRegex(MetadataError, "links and special entries"):
            build_metadata(
                tag=f"v{self.version}",
                source_revision=self.revision,
                artifact_dir=self.root,
                kernel_abi=self.abi,
            )

    def test_rejects_wrong_target_identity_in_build_info(self) -> None:
        self._all_archives()
        target = TARGETS[0]
        archive = self.root / f"cdin-kernel-{self.version}-linux-x86_64.tar.gz"
        archive.unlink()
        (self.root / (archive.name + ".sha256")).unlink()
        wrong = dict(target)
        wrong["abi"] = "gnu-glibc-2.35"
        self._write_archive(wrong)
        with self.assertRaisesRegex(MetadataError, "Target ABI"):
            build_metadata(
                tag=f"v{self.version}",
                source_revision=self.revision,
                artifact_dir=self.root,
                kernel_abi=self.abi,
            )


if __name__ == "__main__":
    unittest.main()
