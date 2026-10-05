"""Logs the ESP32 serial console with timestamps: serial_log.py PORT SECONDS [--reset]"""
import datetime
import sys
import time

import serial

port, seconds = sys.argv[1], float(sys.argv[2])
ser = serial.Serial(port, 115200, timeout=0.5)
if "--reset" in sys.argv:
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
end = time.time() + seconds
buf = b""
while time.time() < end:
    buf += ser.read(4096)
    *lines, buf = buf.split(b"\n")
    for line in lines:
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"[{stamp}] {line.decode(errors='replace').rstrip()}", flush=True)
