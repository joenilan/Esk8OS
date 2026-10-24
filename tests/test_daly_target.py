from pathlib import Path
import os
import subprocess
import tempfile
import unittest


class DalyTargetTests(unittest.TestCase):
    def test_address_validation_and_stationary_policy(self):
        repo = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="evee-daly-target-") as temp:
            binary = str(Path(temp) / "daly-target-test")
            sanitizers = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                          if os.environ.get("EVEE_SANITIZERS") == "1" else [])
            subprocess.run([
                os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
                *sanitizers,
                "-I", str(repo / "src"), str(repo / "tests/daly_target_test.cpp"),
                "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)
