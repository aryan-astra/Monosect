import tempfile
import unittest
from pathlib import Path

from end_to_end import prepare_work_directory


class WorkDirectoryTests(unittest.TestCase):
    def test_unowned_files_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            important = root / "user-data.txt"
            important.write_text("keep")
            with self.assertRaises(ValueError):
                prepare_work_directory(root)
            self.assertEqual("keep", important.read_text())

    def test_owned_work_can_be_recreated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "work"
            prepare_work_directory(root)
            (root / "old-output").write_text("temporary")
            prepare_work_directory(root)
            self.assertFalse((root / "old-output").exists())
            self.assertTrue((root / ".monosect-e2e").exists())

    def test_repository_root_is_never_a_work_directory(self):
        with self.assertRaises(ValueError):
            prepare_work_directory(Path(__file__).resolve().parents[2])


if __name__ == "__main__":
    unittest.main()
