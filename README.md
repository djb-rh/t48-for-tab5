# tab5-burner

An M5Stack Tab5 (with its keyboard) as a stand-alone front end for an XGecu
T48 ROM programmer plugged into the Tab5's USB-A port. The plan is to port
the programmer layer of [minipro](https://gitlab.com/DavidGriffith/minipro)
(GPL-3.0-or-later) and put a touch + keyboard UI on it: part search, blank
check, read, write, verify, a hex editor, and a web file manager for the SD card.

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
