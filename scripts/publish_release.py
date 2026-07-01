#!/usr/bin/env python3
"""Build + publish ESK8OS firmware releases to apps.zombie.digital.

Follows the site's RELEASE_CONTRACT.md: builds the release environments,
packages the .bin artifacts with sha256 sums, writes latest.json/manifest.json/
notes.md, and uploads everything to the Pi's downloads directory over SFTP.

Usage:
  python scripts/publish_release.py                 # build + upload
  python scripts/publish_release.py --dry-run       # build + package only
  python scripts/publish_release.py --notes "..."   # one-line release summary

Credentials come from .env.raspi (SSH_HOST / SSH_USER / SSH_PASSWORD).
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
REMOTE_DIR = f"/mnt/data/sites/apps/public/downloads/{SLUG}"

# env name -> (artifact kind, human label)
RELEASE_ENVS = {
    "tdisplay_s3_ride_release": ("tdisplay-s3", "LilyGo T-Display-S3 (ride release)"),
    "esp32s3_oled_i2c_usb":     ("esp32s3-oled", "ESP32-S3 + I2C OLED"),
    "esp32s3_headless_usb":     ("esp32s3-headless", "ESP32-S3 headless"),
}


def read_env_file(path: pathlib.Path) -> dict:
    env = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        env[k.strip()] = v.strip()
    return env


def sha256_file(p: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="build + package locally, skip upload")
    ap.add_argument("--notes", default="", help="one-line release summary for latest.json")
    ap.add_argument("--skip-build", action="store_true", help="package existing .pio build output")
    args = ap.parse_args()

    version = (REPO / "version.txt").read_text().strip()
    out = REPO / "release" / version
    out.mkdir(parents=True, exist_ok=True)

    # 1. Build every release environment.
    if not args.skip_build:
        for env in RELEASE_ENVS:
            print(f"[build] {env}")
            r = subprocess.run(["pio", "run", "-e", env], cwd=REPO)
            if r.returncode != 0:
                print(f"[build] FAILED: {env}")
                return 1

    # 2. Collect artifacts + sha256.
    files_map = {}
    artifacts = []
    for env, (kind, label) in RELEASE_ENVS.items():
        src = REPO / ".pio" / "build" / env / "firmware.bin"
        if not src.exists():
            print(f"[package] missing {src} — skipping {env}")
            continue
        name = f"esk8os_{version}_{kind}.bin"
        dst = out / name
        shutil.copy2(src, dst)
        digest = sha256_file(dst)
        (out / f"{name}.sha256").write_text(f"{digest}  {name}\n")
        files_map[kind] = name
        artifacts.append({
            "kind": kind,
            "label": label,
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
        "channel": "stable",
        "publishedAt": now,
        "file": primary,
        "notes": args.notes or f"ESK8OS firmware {version}",
        "notesFile": "notes.md",
        "files": files_map,
    }
    manifest = {
        "slug": SLUG,
        "version": version,
        "publishedAt": now,
        "assets": artifacts,
        "install": {
            "ota": "Board web updater: enable WIFI EXPORT from the app (or bridge mode), "
                   "join the board's AP, open http://192.168.4.1 and upload the .bin.",
            "usb": "pio run -e tdisplay_s3_ride_release -t upload (or esptool write_flash)",
        },
    }
    (out / "latest.json").write_text(json.dumps(latest, indent=2) + "\n")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    notes = out / "notes.md"
    if not notes.exists():
        notes.write_text(f"# ESK8OS {version}\n\n- {args.notes or 'Release ' + version}\n")

    print(f"[package] staged in {out}")
    if args.dry_run:
        return 0

    # 4. Upload over SFTP (archive the previous release server-side first).
    env = read_env_file(REPO / ".env.raspi")
    import paramiko  # deferred: only needed for the upload step
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(env["SSH_HOST"], port=int(env.get("SSH_PORT", 22)), username=env["SSH_USER"],
              password=env["SSH_PASSWORD"], allow_agent=False, look_for_keys=False)
    c.exec_command(
        f"mkdir -p {REMOTE_DIR}/archive && "
        f"cd {REMOTE_DIR} && "
        # move any previous version's bins into archive/ (keeps latest.json history simple)
        f"for f in esk8os_*.bin esk8os_*.bin.sha256; do "
        f"  [ -e \"$f\" ] && case \"$f\" in *_{version}_*) ;; *) mv \"$f\" archive/ ;; esac; "
        f"done; true"
    )[1].channel.recv_exit_status()

    sftp = c.open_sftp()
    for p in sorted(out.iterdir()):
        print(f"[upload] {p.name}")
        sftp.put(str(p), f"{REMOTE_DIR}/{p.name}")
    sftp.close()

    # 5. Rebuild the Astro site: nginx serves apps/dist, and `astro build` is what
    #    copies public/downloads (where we just uploaded) into dist.
    print("[site] rebuilding apps.zombie.digital ...")
    i, o, e = c.exec_command(
        "bash -lc 'cd /mnt/data/sites/apps && yarn build' 2>&1 | tail -5")
    build_out = o.read().decode(errors="replace")
    print(build_out.encode(sys.stdout.encoding or "utf-8", "replace").decode(sys.stdout.encoding or "utf-8"))
    c.close()
    print(f"[done] https://apps.zombie.digital/downloads/{SLUG}/latest.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
