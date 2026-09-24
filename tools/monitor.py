#!/usr/bin/env python3
"""Print the Tab5's serial output for N seconds (the port open resets it).

Usage: monitor.py SECONDS [--ready-file=PATH]
--ready-file is touched once the firmware says "ready", so another process
can start its test at the right moment.
"""
import sys
import time

from tab5 import open_port

secs = float(sys.argv[1])
ready = next((a.split("=", 1)[1] for a in sys.argv[2:] if a.startswith("--ready-file=")), None)
s = open_port()
end = time.time() + secs
buf = b""
t0 = time.time()
while time.time() < end:
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode("utf-8", "replace").rstrip()
        print("%6.1f %s" % (time.time() - t0, text), flush=True)
        if ready and text == "ready":
            open(ready, "w").close()
