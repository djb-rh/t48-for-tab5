#!/usr/bin/env python3
"""Build the Tab5's part library from minipro's infoic.xml and logicic.xml.

minipro finds a part by parsing its whole database, ~19 MB of XML, several
times per command. That is fine on a Mac and far too slow from an SD card,
so the library is split for the Tab5:

  parts.idx     one line per searchable name, sorted, tab-separated:
                name  manufacturer  kind  size  offset  length
                (kind: EPROM/EEPROM/Flash... from the entry's type and
                protocol; offset/length: the entry's bytes in entries.xml)
  entries.xml   every T48-capable <ic .../> entry, one per line, verbatim
                apart from whitespace; logic parts keep their <vector>s
  infoic.xml    minipro's infoic.xml with every <ic> removed, and a marker
                comment where the chosen entry goes (INFOIC2PLUS database)
  logicic.xml   the same for logicic.xml

On the Tab5, choosing a part writes a two-file database holding only that
part (skeleton + its entry), which minipro parses in milliseconds.

Only the INFOIC2PLUS database (TL866II+/T48/T56) is used, and only parts the
T48 can program: pin_map bits 0x70000000 say which programmers a part is
limited to (they combine: 0x50000000 is T48 and T56), none set meaning all of
them (database.c, main.c print_device_info).

Usage: mkparts.py <minipro dir> <out dir>
"""
import os
import re
import sys

T56_FLAG, TL866II_FLAG, T48_FLAG = 0x10000000, 0x20000000, 0x40000000
DEVICE_MASK = T56_FLAG | TL866II_FLAG | T48_FLAG
MARKER = "<!--TAB5-PART-->"

# infoic.xml "type" attribute
TYPES = {1: "Memory", 2: "MCU", 3: "PLD", 4: "SRAM", 5: "Logic", 6: "NAND", 7: "eMMC", 8: "VGA"}


def attr(tag, name):
    m = re.search(r'\b' + name + r'="([^"]*)"', tag)
    return m.group(1) if m else None


def num(s):
    return int(s, 0) if s else 0


def compact(tag):
    """One line, single spaces between attributes, content untouched."""
    return re.sub(r"\s*\n\s*", " ", tag.strip())


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    info = open(os.path.join(src, "infoic.xml"), encoding="utf-8").read()
    logic = open(os.path.join(src, "logicic.xml"), encoding="utf-8").read()

    # The TL866II+/T48/T56 database is the one whose type is INFOIC2PLUS.
    dbs = list(re.finditer(r'<database\s+type="([^"]+)"\s*>(.*?)</database\s*>', info, re.S))
    plus = next(m for m in dbs if m.group(1) == "INFOIC2PLUS")

    entries = []   # (entry text, manufacturer, names, kind, size, custom)
    for mm in re.finditer(r'<(manufacturer|custom)\s+name="([^"]*)"\s*>(.*?)</\1\s*>', plus.group(2), re.S):
        custom, mfr = mm.group(1) == "custom", mm.group(2)
        for ic in re.finditer(r"<ic\b.*?/>", mm.group(3), re.S):
            tag = ic.group(0)
            only = num(attr(tag, "pin_map")) & DEVICE_MASK
            if only and not only & T48_FLAG:
                continue
            kind = TYPES.get(num(attr(tag, "type")), "?")
            size = num(attr(tag, "code_memory_size"))
            names = [n for n in attr(tag, "name").split(",") if n]
            entries.append((compact(tag), mfr, names, kind, size, custom))

    lentries = []
    for ic in re.finditer(r"<ic\b[^>]*>.*?</ic>", logic, re.S):
        tag = ic.group(0)
        pins = attr(tag, "pins") or "?"
        entries.append((compact(tag), "Logic IC", [attr(tag, "name")], "Logic", int(pins) if pins.isdigit() else 0, False))
        lentries.append(tag)

    # entries.xml: one entry per line; each index row points at its bytes.
    rows = []
    with open(os.path.join(out, "entries.xml"), "wb") as f:
        for text, mfr, names, kind, size, custom in entries:
            data = text.encode("utf-8")
            off = f.tell()
            f.write(data + b"\n")
            tagged_mfr = mfr + (" (custom)" if custom else "")
            for n in names:
                rows.append((n, tagged_mfr, kind, size, off, len(data)))
    rows.sort(key=lambda r: (r[0].upper(), r[0]))
    with open(os.path.join(out, "parts.idx"), "w", encoding="utf-8") as f:
        for r in rows:
            f.write("%s\t%s\t%s\t%d\t%d\t%d\n" % r)

    # Skeletons: every <ic> gone, the INFOIC2PLUS database reduced to one
    # manufacturer holding the marker. The other databases are dropped: the
    # T48 only ever reads INFOIC2PLUS.
    skel = info[: dbs[0].start()]
    skel += '<database type="INFOIC2PLUS">\n<manufacturer name="Tab5">\n' + MARKER + "\n</manufacturer>\n</database>\n"
    skel += info[dbs[-1].end():]
    open(os.path.join(out, "infoic.xml"), "w", encoding="utf-8").write(skel)
    lskel = re.sub(r"<ic\b[^>]*>.*?</ic>\s*", "", logic, flags=re.S)
    lskel = lskel.replace('<manufacturer name="Logic Ic">', '<manufacturer name="Logic Ic">\n' + MARKER, 1)
    open(os.path.join(out, "logicic.xml"), "w", encoding="utf-8").write(lskel)

    print(f"{len(entries)} entries, {len(rows)} names")
    for n in ("parts.idx", "entries.xml", "infoic.xml", "logicic.xml"):
        print(f"  {n}: {os.path.getsize(os.path.join(out, n))} bytes")


if __name__ == "__main__":
    main()
