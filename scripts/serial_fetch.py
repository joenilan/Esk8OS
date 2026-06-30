"""Fetch a session CSV from the board over serial (COM5) and save it locally.
Usage: python scripts/serial_fetch.py s0001.csv [out_path]"""
import serial, time, sys

PORT = "COM5"
fname = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else f"scripts/_{fname}"

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.3
ser.dtr = False
ser.rts = False
ser.open()
time.sleep(0.4)
ser.reset_input_buffer()

ser.write((f"cat {fname}\n").encode())
ser.flush()

data = b""
end = time.time() + 3.0           # initial grace for the command to start
hard = time.time() + 60.0         # absolute cap
while time.time() < end and time.time() < hard:
    n = ser.in_waiting
    if n:
        data += ser.read(n)
        end = time.time() + 1.5    # extend while bytes keep flowing
    else:
        time.sleep(0.05)
ser.close()

# Strip the echoed command line if present.
text = data.decode(errors="replace")
with open(out, "w", newline="") as f:
    f.write(text)
print(f"saved {len(data)} bytes -> {out}")
print("--- first 5 lines ---")
for ln in text.splitlines()[:5]:
    print(ln)
