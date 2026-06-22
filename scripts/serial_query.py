"""Ad-hoc serial query of the ESK8OS console (COM5). Sends commands and prints
replies. Usage: python scripts/serial_query.py [cmd1] [cmd2] ..."""
import serial, time, sys

PORT = "COM5"
cmds = sys.argv[1:] or ["help", "odo", "free", "logs"]

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.3
ser.dtr = False
ser.rts = False
ser.open()
time.sleep(0.4)
ser.reset_input_buffer()

def cmd(c, wait=1.2):
    ser.write((c + "\n").encode())
    ser.flush()
    end = time.time() + wait
    out = b""
    while time.time() < end:
        n = ser.in_waiting
        if n:
            out += ser.read(n)
            end = time.time() + 0.5
        else:
            time.sleep(0.05)
    print(f"\n>>> {c}")
    sys.stdout.write(out.decode(errors="replace"))

for c in cmds:
    cmd(c, 2.5 if c.startswith(("logs", "cat")) else 1.2)

ser.close()
