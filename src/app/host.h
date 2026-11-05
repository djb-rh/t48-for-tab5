// When the burner runs as one app inside a launcher (Tab5 Multitool), the
// launcher says so here before burner::setup(): a Home button appears in the
// header, and the launcher's Wi-Fi network is used until one is set up here.
// Built on its own (main.cpp), nothing is set and nothing changes.
#pragma once

#include <cstdint>

namespace burner {
namespace host {

struct Config {
  void (*home)() = nullptr;          // back to the launcher (it restarts the Tab5)
  const char *wifi_ssid = nullptr;   // the launcher's network, if it has one
  const char *wifi_pass = nullptr;
  uint32_t blank_ms = 0;             // backlight off after this long idle; 0 = never
};

void set(const Config &c);
const Config &get();
bool hosted();

}  // namespace host
}  // namespace burner
