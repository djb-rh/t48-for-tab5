// What survives a restart, in NVS: the chosen part and image, the write
// options, the One ROM slot settings, and the Wi-Fi network.
#pragma once

#include <string>

namespace burner {
namespace settings {

struct Options {
  bool ignore_size = false;     // -s  a file that is not the chip's size is fine
  bool skip_erase = false;      // -e  do not erase before writing
  bool skip_verify = false;     // -v  do not verify after writing
  bool ignore_id = false;       // -y  carry on when the chip ID does not match
};

// One ROM: how the chosen image is served (one slot, plus the USB plugin).
struct OneRom {
  std::string type = "27C512";   // One ROM chip type name
  int cs[3] = {0, 0, 0};          // configurable select lines: 0 active low, 1 high, 2 ignore
  int fit = 0;                    // an image smaller than the chip: 0 refuse, 1 duplicate, 2 pad
};

struct Settings {
  std::string part_name, part_maker;
  std::string image;            // full path on the card
  Options opt;
  OneRom onerom;
  std::string wifi_ssid, wifi_pass;
};

Settings &get();
void save();

}  // namespace settings
}  // namespace burner
