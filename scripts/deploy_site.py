"""Deploy site/evee to the Pi (evee.zombie.digital). Static hosting — files go
live immediately, no build step. HTML/CSS/JS every run; images only with
--img (they're big and rarely change). Remember: bump ?v=N on css/js
references when those files change — Cloudflare caches them hard.

Usage: python scripts/deploy_site.py [--img]
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SITE = REPO / "site" / "evee"
REMOTE = "/mnt/data/sites/evee/public"


def read_env_file(path):
    env = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    return env


def main():
    include_img = "--img" in sys.argv
    env = read_env_file(REPO / ".env.raspi")
    import paramiko
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(env["SSH_HOST"], port=int(env.get("SSH_PORT", 22)), username=env["SSH_USER"],
              password=env["SSH_PASSWORD"], allow_agent=False, look_for_keys=False)
    sftp = c.open_sftp()

    files = sorted(p for p in SITE.iterdir()
                   if p.is_file() and p.suffix in (".html", ".css", ".js"))
    for p in files:
        print(f"[upload] {p.name}")
        sftp.put(str(p), f"{REMOTE}/{p.name}")

    if include_img:
        c.exec_command(f"mkdir -p {REMOTE}/img")[1].channel.recv_exit_status()
        for p in sorted((SITE / "img").iterdir()):
            if p.is_file():
                print(f"[upload] img/{p.name}")
                sftp.put(str(p), f"{REMOTE}/img/{p.name}")

    sftp.close()
    c.close()
    print(f"deployed {len(files)} file(s) -> https://evee.zombie.digital/")


if __name__ == "__main__":
    main()
