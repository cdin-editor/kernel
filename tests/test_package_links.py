from __future__ import annotations

import hashlib
import os
from pathlib import Path
import tarfile
import subprocess
import sys
import tempfile
import unittest
import zipfile

from scripts.package_registry_prefix import PackageError, write_prefix_archive

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "normalize-package-links.py"



class RegistryPrefixArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.prefix = Path(self.temporary.name) / "prefix"
        self.prefix.mkdir()
        files = {
            "BUILD-INFO.txt": b"Package version: 1.2.3\n",
            "include/cdin/kernel.h": b"kernel header",
            "include/m-mdy-m/log.h": b"logger header",
            "lib/libcdin_kernel.a": b"kernel library",
            "lib/libm-mdy-m-log.a": b"logger library",
            "lib/pkgconfig/cdin-kernel.pc": b"kernel pkgconfig",
            "lib/pkgconfig/m-mdy-m-log.pc": b"logger pkgconfig",
        }
        for relative, content in files.items():
            path = self.prefix / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_tar_gz_starts_at_install_prefix_and_has_real_checksum(self) -> None:
        output = Path(self.temporary.name) / "kernel-cmake-prefix.tar.gz"
        digest, size = write_prefix_archive(self.prefix, output, "tar.gz")
        with tarfile.open(output, "r:gz") as archive:
            names = {member.name.rstrip("/") for member in archive.getmembers()}
        self.assertIn("BUILD-INFO.txt", names)
        self.assertIn("include/cdin/kernel.h", names)
        self.assertIn("lib/libcdin_kernel.a", names)
        self.assertNotIn("prefix/include/cdin/kernel.h", names)
        self.assertEqual(size, output.stat().st_size)
        self.assertEqual(digest, hashlib.sha256(output.read_bytes()).hexdigest())
        self.assertEqual(
            (Path(str(output) + ".sha256")).read_text(encoding="ascii"),
            f"{digest}  {output.name}\n",
        )

    def test_zip_starts_at_install_prefix(self) -> None:
        output = Path(self.temporary.name) / "kernel-cmake-prefix.zip"
        write_prefix_archive(self.prefix, output, "zip")
        with zipfile.ZipFile(output) as archive:
            names = {item.filename.rstrip("/") for item in archive.infolist()}
        self.assertIn("BUILD-INFO.txt", names)
        self.assertIn("include/cdin/kernel.h", names)
        self.assertIn("lib/libm-mdy-m-log.a", names)
        self.assertNotIn("prefix/lib/libcdin_kernel.a", names)

    @unittest.skipIf(os.name == "nt", "POSIX symlink creation may require elevated Windows privileges")
    def test_rejects_symlink_as_package_root(self) -> None:
        link = Path(self.temporary.name) / "prefix-link"
        link.symlink_to(self.prefix, target_is_directory=True)
        with self.assertRaisesRegex(PackageError, "package prefix is not a real directory"):
            write_prefix_archive(
                link,
                Path(self.temporary.name) / "linked-root.tar.gz",
                "tar.gz",
            )

    @unittest.skipIf(os.name == "nt", "POSIX symlink creation may require elevated Windows privileges")
    def test_rejects_symlinks_in_registry_prefix(self) -> None:
        alias = self.prefix / "lib" / "escape"
        alias.symlink_to("../include/cdin/kernel.h")
        with self.assertRaisesRegex(PackageError, "cannot contain symlinks"):
            write_prefix_archive(
                self.prefix,
                Path(self.temporary.name) / "unsafe.tar.gz",
                "tar.gz",
            )


@unittest.skipIf(os.name == "nt", "POSIX symlink creation may require elevated Windows privileges")
class PackageLinkNormalizationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "package"
        self.root.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _run(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(self.root)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_materializes_internal_file_alias_as_regular_copy(self) -> None:
        library = self.root / "lib"
        library.mkdir()
        real = library / "libcdin_kernel.so.1"
        alias = library / "libcdin_kernel.so"
        real.write_bytes(b"kernel shared library")
        real.chmod(0o755)
        alias.symlink_to(real.name)

        result = self._run()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(alias.is_symlink())
        self.assertTrue(alias.is_file())
        self.assertEqual(alias.read_bytes(), real.read_bytes())
        self.assertTrue(alias.stat().st_mode & 0o100)

    def test_rejects_links_that_escape_the_package_root(self) -> None:
        outside = Path(self.temporary.name) / "outside.bin"
        outside.write_bytes(b"outside")
        alias = self.root / "outside-link"
        alias.symlink_to(outside)

        result = self._run()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("escapes package root", result.stderr)
        self.assertTrue(alias.is_symlink())

    def test_rejects_directory_symlinks(self) -> None:
        outside = Path(self.temporary.name) / "outside-dir"
        outside.mkdir()
        alias = self.root / "external"
        alias.symlink_to(outside, target_is_directory=True)

        result = self._run()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("directory symlinks are not permitted", result.stderr)


if __name__ == "__main__":
    unittest.main()
