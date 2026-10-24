#!/usr/bin/env python3
"""Build and stage a local ESK8OS firmware candidate; never upload or deploy.

Use --dry-run to build all six variants and stage checksums and metadata.
Use --require-clean to enforce a committed, unchanged source identity.
Publication belongs to the separate website helper after hardware validation.
"""

import argparse
import datetime
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SLUG = "esk8os-firmware"

# env name -> (artifact kind, human label, BMS driver id)
RELEASE_ENVS = {
    "tdisplay_s3_ride_release": ("tdisplay-s3", "LilyGo T-Display-S3 (ride release)", "none"),
    "tdisplay_s3_bms":          ("tdisplay-s3-bms", "LilyGo T-Display-S3 + Daly BMS (USB serial)", "daly"),
    "esp32s3_oled_i2c_usb":     ("esp32s3-oled", "ESP32-S3 + I2C OLED", "none"),
    "esp32s3_oled_i2c_bms_usb": ("esp32s3-oled-bms", "ESP32-S3 + I2C OLED + Daly BMS", "daly"),
    "esp32s3_headless_usb":     ("esp32s3-headless", "ESP32-S3 headless", "none"),
    "esp32s3_headless_bms_usb": ("esp32s3-headless-bms", "ESP32-S3 headless + Daly BMS", "daly"),
}


def source_identity() -> dict:
    try:
        commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=REPO, stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        dirty = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=REPO, stderr=subprocess.DEVNULL,
            text=True,
        ).strip())
        return {"commit": commit, "dirty": dirty}
    except (OSError, subprocess.CalledProcessError):
        return {"commit": None, "dirty": None}


def sha256_file(p: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="required: build and package locally; publication is separate")
    ap.add_argument("--require-clean", action="store_true",
                    help="require a committed source tree unchanged throughout the build")
    ap.add_argument("--notes", default="", help="one-line release summary for latest.json")
    ap.add_argument("--skip-build", action="store_true", help="package existing .pio build output")
    ap.add_argument("--output-dir", type=pathlib.Path,
                    help="verification staging directory (requires --dry-run)")
    args = ap.parse_args()
    if not args.dry_run:
        ap.error("Live publishing is retired. Use --dry-run to stage a candidate; "
                 "publication requires the separate website process after hardware validation.")
    source = source_identity()
    if args.require_clean and (not source["commit"] or source["dirty"] is not False):
        print("[source] a clean committed source tree is required")
        return 1

    version = (REPO / "version.txt").read_text().strip()
    out = args.output_dir or REPO / "release" / version

    # 1. Build every release environment.
    if not args.skip_build:
        for env in RELEASE_ENVS:
            print(f"[build] {env}")
            r = subprocess.run([sys.executable, "-m", "platformio", "run", "-e", env], cwd=REPO)
            if r.returncode != 0:
                print(f"[build] FAILED: {env}")
                return 1

    if args.require_clean and source_identity() != source:
        print("[source] source identity changed during the build; refusing packaging")
        return 1

    # Require the complete hardware/BMS matrix before touching release staging.
    # In particular --skip-build must never publish a partial release silently.
    missing = [env for env in RELEASE_ENVS
               if not (REPO / ".pio" / "build" / env / "firmware.bin").is_file()]
    # Preserve the bundled font's existing terms. Project licensing is undecided;
    # draft proposals must not be copied into a release or advertised as adopted.
    notices = ("OFL.txt",)
    missing_notices = [name for name in notices if not (REPO / name).is_file()]
    if missing or missing_notices:
        print("[package] missing required inputs: " + ", ".join(missing + missing_notices))
        return 1

    if args.require_clean:
        stamp = f"v{version} {source['commit'][:7]}".encode() + b"\0"
        mismatched = [env for env in RELEASE_ENVS
                      if stamp not in (REPO / ".pio" / "build" / env / "firmware.bin").read_bytes()]
        if mismatched:
            print("[source] binaries do not identify the clean source commit: " + ", ".join(mismatched))
            return 1

    # 2. Collect artifacts + sha256.
    out.mkdir(parents=True, exist_ok=True)
    for name in notices:
        shutil.copy2(REPO / name, out / name)
    files_map = {}
    artifacts = []
    for env, (kind, label, bms_driver) in RELEASE_ENVS.items():
        src = REPO / ".pio" / "build" / env / "firmware.bin"
        name = f"esk8os_{version}_{kind}.bin"
        dst = out / name
        shutil.copy2(src, dst)
        digest = sha256_file(dst)
        (out / f"{name}.sha256").write_text(f"{digest}  {name}\n")
        files_map[kind] = name
        artifacts.append({
            "sourceCommit": source["commit"],
            "kind": kind,
            "label": label,
            "environment": env,
            "bms": bms_driver,
            "file": name,
            "sha256": digest,
            "bytes": dst.stat().st_size,
        })
        print(f"[package] {name} ({dst.stat().st_size} B)")

    if not artifacts:
        print("[package] no artifacts found — nothing to publish")
        return 1

    # 3. Contract metadata.
    now = datetime.datetime.now(datetime.timezone.utc).isoformat()
    primary = files_map.get("tdisplay-s3") or artifacts[0]["file"]
    latest = {
        "version": version,
        "channel": "candidate",
        "source": source,
        "hardwareValidation": "pending",
        "publicationStatus": "pending-hardware-validation",
        "publishedAt": now,
        "file": primary,
        "notes": args.notes or f"ESK8OS firmware {version}",
        "notesFile": "notes.md",
        "files": files_map,
    }
    manifest = {
        "slug": SLUG,
        "channel": "candidate",
        "source": source,
        "hardwareValidation": "pending",
        "publicationStatus": "pending-hardware-validation",
        "version": version,
        "publishedAt": now,
        "assets": artifacts,
        "fontLicenseFile": "OFL.txt",
        "install": {
            "ota": "Board web updater: enable WIFI EXPORT from the app (or bridge mode), "
                   "join the board's AP, open http://192.168.4.1 and upload the .bin.",
            "usb": "Archive session logs first, then pio run -e <environment> -t upload. "
                   "Use the environment listed on the matching hardware/BMS asset.",
        },
    }
    (out / "latest.json").write_text(json.dumps(latest, indent=2) + "\n")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    notes = out / "notes.md"
    if not notes.exists():
        notes.write_text(f"# ESK8OS {version}\n\n- {args.notes or 'Release ' + version}\n")

    print(f"[package] staged in {out}")
    print("[candidate] hardware validation pending; no publication or deployment performed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
