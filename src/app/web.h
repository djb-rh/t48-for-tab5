// Wi-Fi, its setup hotspot, and the browser file manager for /sdcard/burner.
//
// The Tab5's radio is an ESP32-C6 behind esp_hosted, and its transport
// cannot be started twice in one boot, so the radio comes up once and stays
// up; changing networks disconnects and rejoins without powering it down.
#pragma once

#include <string>
#include <vector>

namespace burner {
namespace web {

void begin();          // joins the saved network (if any) and serves once joined
void loop();           // from the main loop

enum class State { Off, Connecting, Connected, Failed };
State state();
std::string ssid();
std::string ip();       // "" until connected
std::string statusText();   // for the header: "off", "joining...", "10.0.1.23"

// Joins another network (and remembers it).
void join(const std::string &ssid, const std::string &pass);
void forget();                  // clears the saved network (next boot: setup)

// The setup hotspot (captive portal): an open network, T48-Burner-XXXX, whose
// page picks a network and password. It opens by itself when there is no
// saved network or joining fails, and closes a minute after joining.
bool portalActive();
std::string portalSsid();
void setupFromPhone();

struct Net {
  std::string ssid;
  int rssi;
  bool open;
};
std::vector<Net> scan();   // blocking, a few seconds

// An image the browser asked to make current; the main loop applies it.
bool takeChosenImage(std::string *path);

}  // namespace web
}  // namespace burner
