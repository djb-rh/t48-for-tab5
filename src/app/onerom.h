// One ROM (piers.rocks, RP2350 "Fire" boards) on the USB-A port: what is on
// it, programming it with an image from the card, reading and verifying what
// it serves, updating its firmware, and the name and notes kept for each one.
//
// A One ROM speaks picoboot: its USB plugin while it runs (live access to the
// ROM it serves at 0x9000_0000), the RP2350's bootloader while it is stopped
// (flash erase and write). Images are built on the Tab5 by One ROM's own
// generator (onerom-ffi, Rust) from the base firmware and USB plugin
// published at images.onerom.org, downloaded once and kept on the card:
//
//   /sdcard/burner/onerom/cache/   manifests, firmware, plugins
//   /sdcard/burner/onerom/devices/ <serial>.txt: name, notes, history
//
// The jobs run in the runner's task (runner::startFn); they report through
// runner::note and runner::setPhase and return 0 on success.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace burner {
namespace onerom {

constexpr const char *kDir = "/sdcard/burner/onerom";
constexpr const char *kCache = "/sdcard/burner/onerom/cache";
constexpr const char *kDevices = "/sdcard/burner/onerom/devices";

struct Slot {
  int index = 0;          // in flash, plugins included
  int user = -1;          // the jumper-selected number; -1 for a plugin
  bool plugin = false;
  bool active = false;    // the one being served
  std::string type, file;
  uint32_t size = 0;
};

// What the firmware parser found on the device.
struct Info {
  bool valid = false;        // a probe has run for this attach
  bool recognised = false;
  bool running = false;
  bool usb = false;          // has the USB plugin (reachable while running)
  std::string board;         // "fire-28-c"
  std::string version;       // "0.6.14"
  std::string mcu, size;     // size "M" or "L"
  std::vector<Slot> slots;
  std::vector<std::string> errors;
};

// The record kept for a serial (a .txt the browser can edit too).
struct Notes {
  std::string name, notes;
  std::string seen, programmed;    // local time, "2026-10-10 14:05"
  std::string image, type;         // what the Tab5 last put on it
  std::string firmware;            // and with which firmware
  std::string board;               // "fire-28-c", for when its flash says nothing
};

// ---- state the UI reads (any task) ----
bool present();                  // running or in a bootloader
bool bootloader();
std::string serial();
uint32_t generation();           // changes on every attach
Info info();                     // the last probe of this attach
std::string latestFirmware();    // newest known for this board ("" before a check)

// Short board name for the screen: "Fire 28 C".
std::string boardLabel(const std::string &board);

// ---- notes ----
bool loadNotes(const std::string &serial, Notes *n);
bool saveNotes(const std::string &serial, const Notes &n);
std::string displayName(const std::string &serial);   // the name, or a short serial

// ---- chip types (One ROM's own list, not minipro's) ----
std::vector<std::string> chipTypes(const std::string &board);
struct ChipType {
  bool ok = false;
  uint32_t size = 0;
  int pins = 0;
  std::vector<std::string> config_lines;   // select lines the user sets ("cs1"...)
  std::vector<bool> may_ignore;
};
ChipType chipType(const std::string &name);

// What to put in the one ROM slot.
struct Program {
  std::string image;     // full path on the card
  std::string type;      // One ROM chip type
  int cs[3] = {0, 0, 0}; // per config line: 0 active low, 1 active high, 2 ignore
  int fit = 0;           // smaller image: 0 refuse, 1 duplicate, 2 pad
  std::string name;      // recorded on the device instead of the file's name
};

// ---- jobs (runner task) ----
int jobProbe(bool quiet);                          // refreshes info()
int jobProgram(const Program &p);                  // build, flash, check
int jobRead(const std::string &path, uint32_t size);
int jobVerify(const std::string &path);
int jobUpdateFirmware();                           // newest firmware, same ROM
int jobIdentify();                                 // flash its LED for a while
int jobCheckUpdates();                             // refresh the manifests

// Called from the UI loop: notices attaches (probe, notes' "seen").
void tick();

// Every One ROM there are notes for, newest "seen" first.
std::vector<std::pair<std::string, Notes>> known();

}  // namespace onerom
}  // namespace burner
