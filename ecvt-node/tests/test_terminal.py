"""Exercise production console parsing, motion bounds and cancellation admission."""
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
            tmp = Path(directory)
            binary = tmp / "terminal"
            (tmp/'esp_err.h').write_text('#pragma once\nusing esp_err_t=int;\nconstexpr int ESP_OK=0;\nconst char *esp_err_to_name(int);\n')
            (tmp/'esp_timer.h').write_text('#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n')
            subprocess.run([
                "c++", "-std=c++17", "-pthread", "-I", str(tmp), "-I", str(PROJECT / "src"),
                str(PROJECT / "src/console/commands.cpp"),
                str(PROJECT / "tests/terminal_harness.cpp"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
