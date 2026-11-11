from __future__ import annotations

import hashlib
import io
from pathlib import Path
import stat
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

    def _package_files(self, target: dict[str, str]) -> dict[str, bytes]:
        files = {
            "BUILD-INFO.txt": self._build_info(target),
            "include/cdin/kernel.h": b"kernel header",
            "include/m-mdy-m/log.h": b"logger header",
            "lib/libcdin_kernel.a": b"kernel archive",
            "lib/libm-mdy-m-log.a": b"logger archive",
            "lib/pkgconfig/cdin-kernel.pc": b"kernel pkgconfig",
            "lib/pkgconfig/m-mdy-m-log.pc": b"logger pkgconfig",
        }
        if target["os"] == "windows":
            files["lib/libcdin_kernel.dll.a"] = b"kernel import library"
            files["bin/cdin_kernel.dll"] = b"kernel dll"
        elif target["os"] == "darwin":
            files[f"lib/libcdin_kernel.{self.abi}.dylib"] = b"kernel dylib"
        else:
            files[f"lib/libcdin_kernel.so.{self.abi}"] = b"kernel shared object"
        return files

    def _expected_filename(self, target: dict[str, str]) -> str:
        return (
            f"cdin-kernel-{self.version}-{target['platform']}-"
            f"{target['architecture']}-cmake-prefix{target['extension']}"
        )

    def _write_archive(
        self,
        target: dict[str, str],
        *,
        unsafe_link: bool = False,
        wrapped: bool = False,
    ) -> Path:
        filename = self._expected_filename(target)
        path = self.root / filename
        prefix = f"cdin-kernel-{self.version}/" if wrapped else ""
        files = self._package_files(target)

        if target["extension"] == ".zip":
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for name, data in sorted(files.items()):
                    archive.writestr(prefix + name, data)
                if unsafe_link:
                    info = zipfile.ZipInfo(prefix + "lib/escape")
                    info.create_system = 3
                    info.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(info, b"../../outside")
        else:
            with tarfile.open(path, "w:gz", format=tarfile.PAX_FORMAT) as archive:
                for name, data in sorted(files.items()):
                    item = tarfile.TarInfo(prefix + name)
                    item.size = len(data)
                    item.mode = 0o644
                    archive.addfile(item, io.BytesIO(data))
                if unsafe_link:
                    item = tarfile.TarInfo(prefix + "lib/escape")
                    item.type = tarfile.SYMTYPE
                    item.linkname = "../../outside"
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
            self.assertIn("-cmake-prefix.", artifact["file_name"])

    def test_rejects_missing_platform_archive(self) -> None:
        self._all_archives()
        missing = self._expected_filename(TARGETS[-1])
        (self.root / missing).unlink()
        (self.root / (missing + ".sha256")).unlink()
        with self.assertRaisesRegex(MetadataError, "exactly five cmake-prefix archives"):
            build_metadata(
                tag=f"v{self.version}",
                source_revision=self.revision,
                artifact_dir=self.root,
                kernel_abi=self.abi,
            )

    def test_rejects_checksum_mismatch(self) -> None:
        self._all_archives()
        sidecar = self.root / (self._expected_filename(TARGETS[0]) + ".sha256")
        archive_name = self._expected_filename(TARGETS[0])
        sidecar.write_text(f"{'0' * 64}  {archive_name}\n", encoding="ascii")
        with self.assertRaisesRegex(MetadataError, "SHA-256 sidecar"):
            build_metadata(
                tag=f"v{self.version}",
                source_revision=self.revision,
                artifact_dir=self.root,
                kernel_abi=self.abi,
            )

    def test_rejects_archive_links(self) -> None:
        self._all_archives()
        archive_name = self._expected_filename(TARGETS[0])
        (self.root / archive_name).unlink()
        (self.root / (archive_name + ".sha256")).unlink()
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
        archive_name = self._expected_filename(target)
        (self.root / archive_name).unlink()
        (self.root / (archive_name + ".sha256")).unlink()
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

    def test_rejects_wrapped_release_archive_instead_of_install_prefix(self) -> None:
        self._all_archives()
        target = TARGETS[0]
        archive_name = self._expected_filename(target)
        (self.root / archive_name).unlink()
        (self.root / (archive_name + ".sha256")).unlink()
        self._write_archive(target, wrapped=True)
        with self.assertRaisesRegex(MetadataError, "expose the install prefix at its root"):
            build_metadata(
                tag=f"v{self.version}",
                source_revision=self.revision,
                artifact_dir=self.root,
                kernel_abi=self.abi,
            )


if __name__ == "__main__":
    unittest.main()
