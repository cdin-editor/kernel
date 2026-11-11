from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "normalize-package-links.py"


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
