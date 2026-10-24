import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "publish_release", Path(__file__).resolve().parents[1] / "scripts/publish_release.py"
)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleasePackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        (self.repo / "version.txt").write_text("0.12.2\n")
        for name in ("OFL.txt",):
            (self.repo / name).write_text(f"{name} test fixture\n")
        self.output = self.repo / "isolated-staging"

    def firmware(self, env):
        return self.repo / ".pio" / "build" / env / "firmware.bin"

    def create_firmware(self):
        for env in release.RELEASE_ENVS:
            path = self.firmware(env)
            path.parent.mkdir(parents=True)
            path.write_bytes(env.encode())

    def run_release(self, *args):
        with patch.object(release, "REPO", self.repo), patch.object(
            sys, "argv", ["publish_release.py", "--dry-run", *args]
        ), patch.object(
            release, "source_identity", return_value={"commit": "a" * 40, "dirty": False}
        ), contextlib.redirect_stdout(io.StringIO()):
            return release.main()

    def test_all_six_combinations_have_distinct_artifacts_and_correct_metadata(self):
        self.create_firmware()
        self.assertEqual(
            self.run_release("--skip-build", "--output-dir", str(self.output)), 0
        )
        latest = json.loads((self.output / "latest.json").read_text())
        manifest = json.loads((self.output / "manifest.json").read_text())
        expected_kinds = {
            hardware + suffix
            for hardware in ("tdisplay-s3", "esp32s3-oled", "esp32s3-headless")
            for suffix in ("", "-bms")
        }
        self.assertEqual(set(latest["files"]), expected_kinds)
        self.assertEqual(latest["channel"], "candidate")
        self.assertEqual(manifest["channel"], "candidate")
        self.assertEqual(latest["hardwareValidation"], "pending")
        self.assertEqual(manifest["hardwareValidation"], "pending")
        self.assertEqual(latest["publicationStatus"], "pending-hardware-validation")
        self.assertEqual(latest["source"], {"commit": "a" * 40, "dirty": False})
        self.assertEqual(manifest["source"], latest["source"])
        self.assertEqual(len(manifest["assets"]), 6)
        self.assertEqual(len(set(latest["files"].values())), 6)
        self.assertEqual(latest["file"], latest["files"]["tdisplay-s3"])
        for asset in manifest["assets"]:
            payload = (self.output / asset["file"]).read_bytes()
            digest = hashlib.sha256(payload).hexdigest()
            self.assertEqual(asset["sourceCommit"], "a" * 40)
            self.assertEqual(payload, asset["environment"].encode())
            self.assertEqual(asset["sha256"], digest)
            self.assertEqual(asset["bytes"], len(payload))
            self.assertEqual(
                asset["bms"], "daly" if asset["kind"].endswith("-bms") else "none"
            )
            self.assertEqual(
                (self.output / (asset["file"] + ".sha256")).read_text(),
                f"{digest}  {asset['file']}\n",
            )
        self.assertFalse((self.repo / "release").exists())
        for name in ("OFL.txt",):
            self.assertEqual((self.output / name).read_bytes(), (self.repo / name).read_bytes())
        self.assertNotIn("license", manifest)
        self.assertNotIn("license", latest)
        self.assertFalse((self.output / "LICENSE").exists())
        self.assertFalse((self.output / "NOTICE").exists())

    def test_missing_font_license_refuses_packaging_before_writing(self):
        self.create_firmware()
        (self.repo / "OFL.txt").unlink()
        self.assertEqual(self.run_release("--skip-build", "--output-dir", str(self.output)), 1)
        self.assertFalse(self.output.exists())

    def test_missing_variant_refuses_partial_release_before_writing(self):
        self.create_firmware()
        self.firmware("esp32s3_headless_bms_usb").unlink()
        self.assertEqual(
            self.run_release("--skip-build", "--output-dir", str(self.output)), 1
        )
        self.assertFalse(self.output.exists())

    def test_missing_variant_preserves_existing_staging(self):
        self.output.mkdir()
        old = self.output / "latest.json"
        old.write_text("previous release\n")
        self.assertEqual(
            self.run_release("--skip-build", "--output-dir", str(self.output)), 1
        )
        self.assertEqual(old.read_text(), "previous release\n")
        self.assertEqual(list(self.output.iterdir()), [old])

    def test_build_failure_stops_before_packaging_using_current_python(self):
        with patch.object(
            release.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)
        ) as build:
            self.assertEqual(self.run_release(), 1)
        build.assert_called_once_with(
            [sys.executable, "-m", "platformio", "run", "-e", "tdisplay_s3_ride_release"],
            cwd=self.repo,
        )
        self.assertFalse((self.repo / "release").exists())

    def test_custom_directory_cannot_be_uploaded(self):
        with patch.object(
            sys, "argv", ["publish_release.py", "--output-dir", str(self.output)]
        ), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            release.main()
        self.assertEqual(error.exception.code, 2)
        self.assertFalse(self.output.exists())

    def test_vendor_metadata_comes_from_the_driver_not_the_filename(self):
        variants = {"example_env": ("example-board-bms", "Example contributed driver", "example-driver")}
        with patch.object(release, "RELEASE_ENVS", variants):
            self.create_firmware()
            self.assertEqual(self.run_release("--skip-build", "--output-dir", str(self.output)), 0)
        manifest = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(manifest["assets"][0]["bms"], "example-driver")

    def test_retired_live_path_fails_before_building_or_packaging(self):
        with patch.object(sys, "argv", ["publish_release.py"]), patch.object(
            release.subprocess, "run"
        ) as run, contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            release.main()
        run.assert_not_called()
        self.assertFalse((self.repo / "release").exists())

    def test_require_clean_refuses_dirty_or_unknown_source(self):
        for identity in ({"commit": "a" * 40, "dirty": True}, {"commit": None, "dirty": None}):
            with patch.object(release, "REPO", self.repo), patch.object(
                release, "source_identity", return_value=identity
            ), patch.object(sys, "argv", ["publish_release.py", "--dry-run", "--require-clean"]), patch.object(
                release.subprocess, "run"
            ) as build, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(release.main(), 1)
            build.assert_not_called()
        self.assertFalse((self.repo / "release").exists())

    def test_require_clean_refuses_source_changes_during_build(self):
        before = {"commit": "a" * 40, "dirty": False}
        after = {"commit": "a" * 40, "dirty": True}
        with patch.object(release, "REPO", self.repo), patch.object(
            release, "source_identity", side_effect=[before, after]
        ), patch.object(sys, "argv", ["publish_release.py", "--dry-run", "--require-clean", "--skip-build"]), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(release.main(), 1)
        self.assertFalse((self.repo / "release").exists())

    def test_clean_candidate_packages_with_full_commit_identity(self):
        self.create_firmware()
        for env in release.RELEASE_ENVS:
            with self.firmware(env).open("ab") as binary:
                binary.write(b"\0v0.12.2 aaaaaaa\0")
        self.assertEqual(self.run_release("--require-clean", "--skip-build", "--output-dir", str(self.output)), 0)
        latest = json.loads((self.output / "latest.json").read_text())
        self.assertFalse(latest["source"]["dirty"])
        self.assertEqual(len(latest["source"]["commit"]), 40)

    def test_clean_source_cannot_relabel_stale_or_dirty_binaries(self):
        self.create_firmware()
        for suffix in (b"\0v0.12.2 bbbbbbb\0", b"\0v0.12.2 aaaaaaa-dirty\0"):
            for env in release.RELEASE_ENVS:
                self.firmware(env).write_bytes(env.encode() + suffix)
            self.assertEqual(self.run_release("--require-clean", "--skip-build", "--output-dir", str(self.output)), 1)
            self.assertFalse(self.output.exists())

    def test_cold_build_seeds_generated_header_before_platformio_fingerprints(self):
        self.create_firmware()
        header = self.repo / "src/version.h"
        self.assertFalse(header.exists())
        def build(*args, **kwargs):
            self.assertTrue(header.is_file())
            return subprocess.CompletedProcess(args[0], 0)
        with patch.object(release.subprocess, "run", side_effect=build) as run:
            self.assertEqual(self.run_release("--output-dir", str(self.output)), 0)
        self.assertEqual(run.call_count, 6)

    def test_existing_generated_header_is_not_rewritten_or_retouched(self):
        self.create_firmware()
        header = self.repo / "src/version.h"
        header.parent.mkdir()
        header.write_text("existing generated identity\n")
        original_mtime = header.stat().st_mtime_ns
        with patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            self.assertEqual(self.run_release("--output-dir", str(self.output)), 0)
        self.assertEqual(header.read_text(), "existing generated identity\n")
        self.assertEqual(header.stat().st_mtime_ns, original_mtime)


if __name__ == "__main__":
    unittest.main()
