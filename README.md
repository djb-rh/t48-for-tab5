# tab5-burner

An M5Stack Tab5 (with its keyboard) as a stand-alone front end for an XGecu
T48 ROM programmer plugged into the Tab5's USB-A port. The plan is to port
the programmer layer of [minipro](https://gitlab.com/DavidGriffith/minipro)
(GPL-3.0-or-later) and put a touch + keyboard UI on it: part search, blank
check, read, write, verify, a hex editor, and a web file manager for the SD card.

## The app (`pio run -e app -t upload`, the default env)

Tab5 + keyboard + T48 on USB-A + SD card. Everything lives under
`/sdcard/burner` on a **FAT32** card (the prebuilt ESP-IDF has exFAT off; the
console's `sdformat ERASE` formats one, erasing it):

    burner/db/      the part library, built on the Mac by tools/mkparts.py
    burner/sel/     minipro's one-part database for the chosen chip
    burner/images/  ROM images: reads land here, writes come from here

Main screen keys: P choose chip (type to search 30,043 T48 parts), D chip
info, F choose image, H hex editor, B blank check, R read, W write, V verify,
E erase, I chip ID, T logic test (logic parts), 1-4 write options (size
mismatch OK / skip erase / skip verify / ignore ID mismatch), N Wi-Fi.
Everything is also a touch button. Battery charging pauses during every job.

Hex editor: arrows, PgUp/PgDn, Home/End, Ctrl+Home/End; type hex (or text
on the ASCII side, Tab switches); Ctrl+G go to, Ctrl+F find (hex bytes or
"text"), Ctrl+N next, Ctrl+Z undo, Ctrl+S save, Ctrl+A save as, Esc close.
Changed bytes are orange; the status line shows the CRC32.

Wi-Fi: joins the saved network (first boot: include/secrets.h, gitignored)
and serves a file manager at http://<ip>/ for /burner: upload (drag and
drop), download, rename, delete, folders, and "Burn this" to make a file the
current image. ~300 KB/s up. The N screen scans and switches networks.

Updating the part library: `python3 tools/mkparts.py third_party/minipro
sd/burner/db`, upload the four files to burner/db in the browser, restart.

Testing without touching the Tab5: `tools/tab5.py` runs one serial session
(the port open resets the board) of console commands, `--key=`, `--tap=`,
`--shot=`, `--put=`, `--get=`; `tools/monitor.py` just listens.

### Lessons
- **Internal RAM feeds the Wi-Fi transport.** With minipro's 48 KB stack and
  a few 16 KB buffers in internal RAM, an upload left 4 KB of DMA memory in
  one piece and the network died for good. Big buffers and that stack live
  in PSRAM now (153 KB of DMA memory free at boot); `mem` shows it.
- minipro takes the part database from `--infoic/--logicic`; a one-part file
  parses in milliseconds where the full 19 MB XML would take seconds.
- Reads and writes run at the Mac's speed: a 64 KB read in 355 ms, an
  M27C512 write in 31.0 s (Mac 31.2 s). Of a 32.4 s write job, 30.9 s is the
  T48 programming (the per-block status reply waits ~53 ms), 1.1 s other USB,
  0.4 s minipro/SD/firmware; every job prints this breakdown to serial. The
  chip test's 41 s write and 0.93 s read came from its screen redraw holding
  a lock minipro's output path needed.
- ESP-IDF logging is off: with the Mac attached but nothing reading serial,
  uploads intermittently killed the network.

## Phase 0 spike (`pio run -e spike -t upload`)

Proves the hardware path. Result on 2026-09-24:

- The USB-A socket is on the P4's high-speed PHY (schematic: USB2_OTG_D± →
  USB_HOST_DP/DM → J10). Its 5 V comes through an MT9700 load switch
  (U27, R_SET 5.1 kΩ, about 1.3 A if it follows the SY6280's 6800/R).
- The T48 powers up from the port and enumerates at **480 Mbps**:
  `a466:0a53`, one vendor interface, bulk EP 01/81 (commands) and 02/82
  (payload), all 512-byte packets. It asks for 100 mA.
- minipro's "get system info" (5 zero bytes out on EP 01, reply on EP 81)
  comes back in ~20 ms: firmware 00.1.03, type 7 (T48), link 480 Mbps.
- The T48's own supply reading (`minipro --version` prints it) is 5.18 V on
  the Mac. On the Tab5 (USB-C from the Mac) it is **4.37 V while the battery
  charges (~695 mA) and 4.97 V with charging off**, repeatably
  (`tools/charge_test.py`, serial `c` toggles CHG_EN). So the burner pauses
  charging (`M5.Power.setBatteryCharge(false)`) for any chip operation.

The spike's status line shows the T48's supply reading and the INA226
battery current live, so it can be watched on battery. Tap the screen for the
full system info again; serial `i`/`d`/`p`/`u` are in `src/spike/main.cpp`.

### Notes for the port
- The T48 path in minipro only ever uses EP 02 with `limit = 0`
  (`t48_read_block`/`t48_write_block`), so minipro's two-endpoint split
  transfers (TL866II+ only) are not needed.
- IN transfers on ESP-IDF must be a whole number of max-size packets
  (`usb_round_up_to_mps`); the reply is shorter and that is fine.
- `tools/serial_read.py` opens the console without resetting into download
  mode (rts/dtr False before open).

## Phase 1 chip test (`pio run -e chiptest -t upload`, then `-t uploadfs`)

minipro runs on the Tab5. Its sources are built unchanged except `main.c`
(its `main` renamed and driven with an argv, `src/chiptest/minipro_main.c`)
and `usb_nix.c` (replaced by `src/chiptest/usb_esp.cpp`). The database is
`data/infoic.xml`, every 27C EPROM cut out of minipro's by
`tools/trim_infoic.py`, on LittleFS at `/fs`. `tools/mp.py` sends commands
over serial (`m <minipro args>`, `ls`, `crc`, `get`, `rm`); a tap on the
screen runs a preset command for tests on battery.

Results on 2026-09-24 (T48 firmware 00.1.03):

| Test | Tab5 | Mac (minipro 0.7.4) |
|---|---|---|
| Read TMS27C512, 64 KB | 0.93 s, CRC32 6252048b | 0.37 s, identical bytes |
| Blank check / verify | correct both ways | - |
| Write M27C512 (ST) | 41.2 s, verify OK, read-back CRC 6252048b | 31.2 s, verify OK |
| Write TMS27C512 blank | fails at 0x0001 | fails identically: bad blank |

Battery charging is paused while the T48 powers up (it browned the Tab5
out once on a laptop port) and during every command (4.98 V at the T48
instead of 4.37 V).
