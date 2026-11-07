"""Archive safety tests: inspection must reject unsafe tar members without extraction."""
import io
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSPECTOR = ROOT / "scripts" / "inspect-bdfs-tar.py"


def run_inspector(archive):
    return subprocess.run(
        [sys.executable, str(INSPECTOR), str(archive)],
        text=True, capture_output=True, check=False,
    )


class TarSafetyTests(unittest.TestCase):
    def test_accepts_regular_file(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "safe.tar"
            with tarfile.open(archive, "w") as tar:
                info = tarfile.TarInfo("root/file.txt")
                info.size = 4
                tar.addfile(info, io.BytesIO(b"safe"))
            result = run_inspector(archive)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_parent_traversal(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "traversal.tar"
            with tarfile.open(archive, "w") as tar:
                info = tarfile.TarInfo("../escape")
                info.size = 1
                tar.addfile(info, io.BytesIO(b"x"))
            result = run_inspector(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("unsafe path", result.stderr)

    def test_rejects_absolute_path(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "absolute.tar"
            with tarfile.open(archive, "w") as tar:
                info = tarfile.TarInfo("/etc/unsafe")
                info.size = 1
                tar.addfile(info, io.BytesIO(b"x"))
            self.assertEqual(run_inspector(archive).returncode, 1)

    def test_rejects_symlink_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "symlink.tar"
            with tarfile.open(archive, "w") as tar:
                info = tarfile.TarInfo("root/link")
                info.type = tarfile.SYMTYPE
                info.linkname = "../../outside"
                tar.addfile(info)
            result = run_inspector(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("escapes archive root", result.stderr)

    def test_rejects_hardlink_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "hardlink.tar"
            with tarfile.open(archive, "w") as tar:
                info = tarfile.TarInfo("root/link")
                info.type = tarfile.LNKTYPE
                info.linkname = "../outside"
                tar.addfile(info)
            result = run_inspector(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("escapes archive root", result.stderr)

    def test_rejects_device_node(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "device.tar"
            with tarfile.open(archive, "w") as tar:
                info = tarfile.TarInfo("root/device")
                info.type = tarfile.CHRTYPE
                tar.addfile(info)
            result = run_inspector(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("special file type", result.stderr)

    def test_rejects_duplicate_member(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "duplicate.tar"
            with tarfile.open(archive, "w") as tar:
                for _ in range(2):
                    info = tarfile.TarInfo("root/file")
                    info.size = 1
                    tar.addfile(info, io.BytesIO(b"x"))
            result = run_inspector(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("duplicate path", result.stderr)

    def test_rejects_malformed_tar(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "broken.tar"
            archive.write_bytes(b"not a tar archive")
            result = run_inspector(archive)
            self.assertEqual(result.returncode, 1)
            self.assertIn("malformed", result.stderr)


if __name__ == "__main__":
    unittest.main()
