// What survives a restart, in NVS: the chosen part and image, the write
// options, and the Wi-Fi network.
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

struct Settings {
  std::string part_name, part_maker;
  std::string image;            // full path on the card
  Options opt;
  std::string wifi_ssid, wifi_pass;
};

Settings &get();
void save();

}  // namespace settings
}  // namespace burner
