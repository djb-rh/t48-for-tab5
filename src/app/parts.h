// The part library on the SD card (built on the Mac by tools/mkparts.py).
//
//   /sdcard/burner/db/parts.idx     one line per name, sorted: name, maker,
//                                   kind, size, offset and length of its
//                                   entry in entries.xml
//   /sdcard/burner/db/entries.xml   every T48-capable minipro <ic> entry
//   /sdcard/burner/db/infoic.xml    minipro's databases with the entries
//   /sdcard/burner/db/logicic.xml   cut out and a marker in their place
//
// Choosing a part writes /sdcard/burner/sel/{infoic,logicic}.xml: the
// skeletons with just that entry in. minipro is pointed at those with
// --infoic/--logicic and parses a hundred kilobytes, not nineteen megabytes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace burner {
namespace parts {

constexpr const char *kDbDir = "/sdcard/burner/db";
constexpr const char *kSelInfoic = "/sdcard/burner/sel/infoic.xml";
constexpr const char *kSelLogicic = "/sdcard/burner/sel/logicic.xml";

struct Row {
  const char *name;
  const char *maker;
  const char *kind;   // Memory, MCU, PLD, SRAM, Logic, NAND...
  uint32_t size;      // bytes of code memory (pins, for logic parts)
  uint32_t offset, length;
};

// Loads the index into PSRAM. False (with a reason) if the card has none.
bool load(std::string *why);
bool loaded();
int count();
const Row &row(int i);

// Rows whose name contains every space-separated word of the query, case
// ignored; names that start with the first word come first. An empty query
// lists everything.
void search(const std::string &query, std::vector<int> *out);

// Makes row i the chosen part (writes the one-part database).
bool select(int i, std::string *why);
// Finds a row by name and maker, for restoring the choice at boot.
int find(const char *name, const char *maker);

// Pretty size: "64 KB", "2 MB", "512 B"; logic parts: "14 pins".
std::string sizeText(const Row &r);

}  // namespace parts
}  // namespace burner
