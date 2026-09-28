// Unpacking .zip files on the SD card (ROM sets and image bundles usually
// arrive zipped). Stored and deflated entries, CRC-checked; no zip64, no
// encryption. Inflate is the ESP32-P4 ROM's miniz (tinfl), so it costs no
// flash.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace burner {
namespace zipx {

struct Result {
  bool ok = false;
  int files = 0;
  uint64_t bytes = 0;
  std::string error;     // why it stopped, when !ok
};

// Extracts every entry of zip_path under dest_dir (created as needed). Paths
// in the zip are kept but never escape dest_dir. progress(name, i, n) is
// called before each entry.
Result extract(const std::string &zip_path, const std::string &dest_dir,
               std::function<void(const std::string &name, int i, int n)> progress = nullptr);

// "roms.zip" -> "roms"
std::string stem(const std::string &file_name);
bool isZip(const std::string &name);

}  // namespace zipx
}  // namespace burner
