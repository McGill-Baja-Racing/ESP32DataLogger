from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]


class TerminalTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
    def test_parser_limits_and_cancellation_admission(self):
        with tempfile.TemporaryDirectory(prefix="ecvt-terminal-") as directory:
            binary = Path(directory) / "terminal"
            subprocess.run([
                "c++", "-std=c++17", "-pthread", "-I", str(PROJECT / "src"),
                str(PROJECT / "src/console/command_parser.cpp"),
                str(PROJECT / "tests/terminal_harness.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
