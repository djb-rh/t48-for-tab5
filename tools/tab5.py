#!/usr/bin/env python3
"""Drive the burner firmware over the Tab5's USB serial, as one session.

Opening the port resets the Tab5 (and so does closing it), so everything for
one session goes in one invocation, in order:

  tab5.py "ls /sdcard/burner"                    console command, waits for it
  tab5.py --put=sd/burner/db:/sdcard/burner/db   copy a file or folder to the card
  tab5.py --get=/sdcard/burner/images/x.bin:x.bin
  tab5.py --tap=640,300 --key="27c512{enter}" --sleep=1 --shot=main.png

--key takes console key syntax: {enter} {esc} {tab} {bs} {del} {up} {down}
{left} {right} {pgup} {pgdn} {home} {end} and {^x} for Ctrl+x.

rts/dtr are set False before open, or the P4 is held in reset.
"""
import glob
import os
import struct
import sys
import time
import zlib

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


class Link:
    def __init__(self):
        self.s = open_port()
        self.buf = b""

    def readline(self, deadline):
        while b"\n" not in self.buf:
            if time.time() > deadline:
                raise TimeoutError("no reply")
            self.buf += self.s.read(4096)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("utf-8", "replace").rstrip("\r")

    def read_exact(self, n, deadline):
        while len(self.buf) < n:
            if time.time() > deadline:
                raise TimeoutError("short read")
            self.buf += self.s.read(max(4096, n - len(self.buf)))
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def wait_ready(self):
        deadline = time.time() + 30
        while True:
            line = self.readline(deadline)
            if line == "ready":
                return
            if line and not line.startswith(("ESP-ROM", "Build:", "rst:", "Core", "SPI", "load:", "entry", "E (")):
                print("  |", line)

    def command(self, line, timeout=900, echo=True):
        self.s.write(line.encode() + b"\n")
        out = []
        deadline = time.time() + timeout
        while True:
            l = self.readline(deadline)
            if l.startswith("== done"):
                return int(l.split()[2]), out
            out.append(l)
            if echo:
                print("  |", l)

    def shot(self, path):
        self.s.write(b"shot\n")
        deadline = time.time() + 60
        while True:
            l = self.readline(deadline)
            if l.startswith("SHOT "):
                break
        w, h = map(int, l.split()[1:3])
        raw = self.read_exact(w * h * 2, deadline)
        self.readline(deadline)          # ENDSHOT
        self.readline(deadline)          # == done
        rows = []
        for y in range(h):
            px = struct.unpack_from("<%dH" % w, raw, y * w * 2)
            row = bytearray()
            for v in px:
                row += bytes(((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
            rows.append(b"\x00" + bytes(row))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")
        open(path, "wb").write(png)
        print("  shot ->", path)

    def put_file(self, local, remote):
        size = os.path.getsize(local)
        self.s.write(("put %s %d\n" % (remote, size)).encode())
        deadline = time.time() + 10
        while self.readline(deadline) != "put: ready":
            pass
        t0 = time.time()
        with open(local, "rb") as f:
            while True:
                chunk = f.read(4096)
                if not chunk:
                    break
                self.s.write(chunk)
                self.wait_ack(time.time() + 10)
        while True:
            l = self.readline(time.time() + 30)
            if l.startswith("put:"):
                print("  %s (%.1f s)" % (l, time.time() - t0))
            if l.startswith("== done"):
                return int(l.split()[2])

    def wait_ack(self, deadline):
        """Consume bytes up to one ACK (0x06); log text may arrive meanwhile."""
        while b"\x06" not in self.buf:
            if time.time() > deadline:
                raise SystemExit("put: no acknowledgement")
            self.buf += self.s.read(64)
        i = self.buf.index(b"\x06")
        self.buf = self.buf[:i] + self.buf[i + 1:]

    def put(self, local, remote):
        if os.path.isdir(local):
            for name in sorted(os.listdir(local)):
                if not name.startswith("."):
                    self.put(os.path.join(local, name), remote.rstrip("/") + "/" + name)
        else:
            if self.put_file(local, remote):
                raise SystemExit("put failed: " + local)

    def get(self, remote, local):
        rc, out = self.command("get " + remote, echo=False)
        data = bytearray()
        size = crc = None
        for l in out:
            if l.startswith("BEGIN "):
                size = int(l.split()[1])
            elif l.startswith("END "):
                crc = int(l.split()[1], 16)
            elif size is not None and crc is None:
                data += bytes.fromhex(l)
        if rc or size != len(data) or zlib.crc32(data) != crc:
            raise SystemExit("get failed: rc=%s size=%s got=%d" % (rc, size, len(data)))
        open(local, "wb").write(data)
        print("  %s -> %s: %d bytes, crc32 %08x" % (remote, local, len(data), crc))


def main():
    link = Link()
    link.wait_ready()
    rc = 0
    for a in sys.argv[1:]:
        if a.startswith("--tap="):
            x, y = a[6:].split(",")
            link.command("tap %s %s" % (x, y), echo=False)
            time.sleep(0.4)
        elif a.startswith("--key="):
            link.command("key " + a[6:], echo=False)
            time.sleep(0.4)
        elif a.startswith("--sleep="):
            time.sleep(float(a[8:]))
        elif a.startswith("--shot="):
            link.shot(a[7:])
        elif a.startswith("--put="):
            local, remote = a[6:].split(":", 1)
            link.put(local, remote)
        elif a.startswith("--get="):
            remote, local = a[6:].split(":", 1)
            link.get(remote, local)
        else:
            rc, _ = link.command(a)
    sys.exit(rc)


if __name__ == "__main__":
    main()
