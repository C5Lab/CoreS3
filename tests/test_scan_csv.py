"""Host test for scan_networks / show_scan_results CSV parsing. No ESP-IDF."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ScanCsvTests(unittest.TestCase):
    def test_rfc4180_scan_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = str(Path(tmp) / "scan_csv")
            cmd = [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "main"),
                str(ROOT / "main" / "scan_csv.c"),
                str(ROOT / "tests" / "scan_csv_test.c"),
                "-o", executable,
            ]
            subprocess.run(cmd, check=True)
            output = subprocess.check_output([executable], text=True)
            self.assertIn("scan csv PASS", output)


if __name__ == "__main__":
    unittest.main()
