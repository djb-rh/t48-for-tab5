// App state shared between screens, and the ways to move between them.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace burner {
namespace app {

constexpr const char *kImages = "/sdcard/burner/images";

struct Image {
  std::string path;       // "" when none is chosen
  bool ok = false;        // the file exists and was read
  uint32_t size = 0;
  uint32_t crc = 0;
};

void begin();                        // restores the saved part and image
const Image &image();
void setImage(const std::string &path);
void refreshImage();                 // re-read size/CRC (after a read or a save)

int part();                          // row in parts::, or -1
bool setPart(int row, std::string *why);

// Short status strings for the header, refreshed by main.cpp.
struct Status {
  bool t48 = false;
  bool sd = false;
  uint64_t sd_free = 0;
  std::string wifi;                  // "off", "connecting", "10.0.1.23"
  int battery = -1;                  // percent
  bool charging = false;
};
Status &status();

void goMain();
void goParts();
void goFiles();
void goHex();
void goWifi();

void prompt(const std::string &title, const std::string &initial,
            std::function<void(bool ok, const std::string &text)> done);
void confirm(const std::string &title, const std::string &body, const std::string &yes,
             std::function<void(bool yes)> done);

std::string baseName(const std::string &path);
std::string bytesText(uint64_t n);   // "65,536 bytes" / "2.1 MB"

}  // namespace app
}  // namespace burner
