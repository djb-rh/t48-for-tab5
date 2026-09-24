#!/usr/bin/env python3
"""Read the Tab5's USB-CDC console.

The one thing that matters here: pyserial asserts DTR and RTS when it opens a
port, and on the ESP32-P4's USB-Serial/JTAG peripheral RTS is wired to the chip
reset and DTR to the boot pin. Opening the port the ordinary way therefore holds
the Tab5 in reset (silent) or drops it into download mode. Both must be set
False *before* open(), not after.

Usage: serial_read.py [seconds] [--reset]
"""
import glob
import sys
import time

import serial


def open_quiet(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.3
    s.rts = False
    s.dtr = False
    s.open()
    return s


def hard_reset(s):
    """esptool's USB-JTAG reset sequence, ending with both lines idle."""
    s.setDTR(False); s.setRTS(False); time.sleep(0.1)
    s.setRTS(True);  s.setDTR(False); s.setRTS(True); time.sleep(0.1)
    s.setDTR(False); s.setRTS(False)


def main():
    secs = 20.0
    reset = False
    for a in sys.argv[1:]:
        if a == "--reset":
            reset = True
        else:
            secs = float(a)

    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* found")
    s = open_quiet(ports[0])
    if reset:
        hard_reset(s)
    end = time.time() + secs
    while time.time() < end:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode("utf-8", "replace"))
            sys.stdout.flush()
    s.close()


if __name__ == "__main__":
    main()
