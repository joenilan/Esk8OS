"""Archive ALL board session logs to the PC before doing anything else.

RUN THIS BEFORE EVERY FLASH. Every boot creates a session file and rotation
can evict ride data; a ride log needed for a pack-cutoff investigation was
destroyed by bench boots on 2026-07-03 because nobody archived first.

Usage: python scripts/archive_sessions.py [COM5]
Saves to session_archive/<YYYYMMDD-HHMMSS>/*.csv (gitignored).
"""
import re, sys, time, pathlib, datetime
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM5"
REPO = pathlib.Path(__file__).resolve().parent.parent

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.3
ser.dtr = False          # don't reset the board (that would create a new session file)
ser.rts = False
ser.open()
time.sleep(0.4)
ser.reset_input_buffer()

def cmd(c, quiet_after=0.8, max_wait=30.0):
    ser.write((c + "\n").encode())
    ser.flush()
    out = b""
    deadline = time.time() + quiet_after
    hard_stop = time.time() + max_wait
    while time.time() < deadline and time.time() < hard_stop:
        n = ser.in_waiting
        if n:
            out += ser.read(n)
            deadline = time.time() + quiet_after
        else:
            time.sleep(0.05)
    return out.decode(errors="replace")

listing = cmd("logs")
names = re.findall(r"(s\d{4}\.csv)\s+\d+\s*B", listing)
if not names:
    print("no session files found on the board:")
    print(listing)
    sys.exit(1)

dest = REPO / "session_archive" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
dest.mkdir(parents=True, exist_ok=True)

for name in names:
    raw = cmd(f"cat {name}", max_wait=120.0)
    m = re.search(r"-{5}.*?-{5}\r?\n(.*)\r?\n-{5} end -{5}", raw, re.S)
    body = m.group(1) if m else raw
    body = body.replace("\r\n", "\n")
    (dest / name).write_text(body, encoding="utf-8")
    print(f"  saved {name} ({len(body)} B)")

ser.close()
print(f"archived {len(names)} file(s) to {dest}")
