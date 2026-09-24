#!/usr/bin/env python3
"""Drive the chip-test firmware over the Tab5's USB serial.

Opening the port resets the Tab5 (and so does closing it), so every command
for one session goes in one invocation: the script waits for the "ready"
line, sends each command in turn and prints its output up to "== done".

  mp.py "m -p TMS27C512@DIP28 -r /fs/read.bin" "crc /fs/read.bin"
  mp.py --get /fs/read.bin read_tab5.bin     # copy a file off the card

rts/dtr are set False before open, or the P4 is held in reset.
"""
import glob
import sys
import time

import serial


def open_port():
    s = serial.Serial()
    s.port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
    s.baudrate = 115200
    s.timeout = 0.2
    s.rts = False
    s.dtr = False
    s.open()
    return s


def lines(s, deadline):
    buf = b""
    while time.time() < deadline:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode("utf-8", "replace").rstrip("\r")
    raise TimeoutError


def wait_ready(s):
    for line in lines(s, time.time() + 25):
        print(line, flush=True)
        if line.startswith("ready:"):
            return


def run(s, cmd, timeout=600, echo=True):
    s.write(cmd.encode() + b"\n")
    out = []
    for line in lines(s, time.time() + timeout):
        if line.startswith("== done"):
            return int(line.split()[2]), out
        out.append(line)
        if echo:
            print(line, flush=True)


def get(s, remote, local):
    rc, out = run(s, "get " + remote, echo=False)
    data = bytearray()
    size = crc = None
    for line in out:
        if line.startswith("BEGIN "):
            size = int(line.split()[1])
        elif line.startswith("END "):
            crc = int(line.split()[1], 16)
        elif size is not None and crc is None and line and all(c in "0123456789abcdef" for c in line):
            data += bytes.fromhex(line)
    import zlib
    if size is None or len(data) != size or zlib.crc32(data) != crc:
        sys.exit(f"transfer failed: rc={rc} size={size} got={len(data)}")
    open(local, "wb").write(data)
    print(f"{remote} -> {local}: {len(data)} bytes, crc32 {crc:08x}")


def main():
    args = sys.argv[1:]
    s = open_port()
    wait_ready(s)
    if args[:1] == ["--get"]:
        get(s, args[1], args[2])
        return
    rc = 0
    for cmd in args:
        rc, _ = run(s, cmd)
    sys.exit(rc)


if __name__ == "__main__":
    main()
