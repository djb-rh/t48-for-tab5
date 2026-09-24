#!/usr/bin/env python3
"""Cut minipro's infoic.xml down to a handful of parts for the flash filesystem.

The full database (~19 MB) will live on the SD card eventually. Until then the
chip test keeps every section minipro parses (configurations, pin maps, the
database skeletons) and drops each <ic .../> whose name does not match.

Usage: trim_infoic.py <infoic.xml> <out.xml> <regex> [<regex> ...]
"""
import re
import sys

src, out, pats = sys.argv[1], sys.argv[2], [re.compile(p, re.I) for p in sys.argv[3:]]
text = open(src, encoding="utf-8").read()
kept = 0


def keep(m):
    global kept
    name = re.search(r'name="([^"]*)"', m.group(0))
    if name and any(p.search(name.group(1)) for p in pats):
        kept += 1
        return m.group(0)
    return ""


trimmed = re.sub(r"[ \t]*<ic\b.*?/>\n?", keep, text, flags=re.S)
# A manufacturer left with no parts is harmless to minipro; leave it.
open(out, "w", encoding="utf-8").write(trimmed)
print(f"{kept} parts kept, {len(text)} -> {len(trimmed)} bytes")
