#!/usr/bin/env python3
"""Watch the T48's supply reading with the Tab5's battery charging on, off, on.

Holds the port open throughout: opening or closing it resets the Tab5.
"""
import glob, sys, time
import serial

s = serial.Serial()
s.port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
s.baudrate = 115200
s.timeout = 0.2
s.rts = False
s.dtr = False
s.open()

def watch(secs):
    end = time.time() + secs
    while time.time() < end:
        d = s.read(4096)
        if d:
            for line in d.decode("utf-8", "replace").splitlines():
                if line.startswith("power:") or "charging" in line or "T48" in line:
                    print(line, flush=True)

steps = [float(a) for a in sys.argv[1:]] or [10, 15, 15]
print("--- boot, charging on"); watch(steps[0])
s.write(b"c"); print("--- charging OFF"); watch(steps[1])
s.write(b"c"); print("--- charging ON"); watch(steps[2])
s.close()
