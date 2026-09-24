#include "sdcard.h"

#include <M5Unified.h>
#include <SD_MMC.h>

namespace burner {
namespace sdcard {
namespace {

constexpr const char *kMount = "/sdcard";
bool g_mounted = false;
const char *g_width = "";

}  // namespace

bool begin() {
  if (g_mounted) return true;

  // The board definition names the slot's pins in SPI terms; on the Tab5 they
  // are the SDIO lines, which is how SD_MMC wants them.
  const int clk = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  const int cmd = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  const int d0 = M5.getPin(m5::pin_name_t::sd_spi_miso);
  const int d3 = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (clk < 0 || cmd < 0 || d0 < 0) return false;

  // The Tab5 wires all four data lines (D1 and D2 sit between D0 and D3), so
  // try the wide bus first: reading a ROM directory of thousands of entries
  // is bandwidth-bound. Fall back to one bit if the slot disagrees.
  if (d3 == d0 + 3) {
    SD_MMC.setPins(clk, cmd, d0, d0 + 1, d0 + 2, d3);
    if (SD_MMC.begin(kMount, false, false, 20000)) {
      g_mounted = true;
      g_width = "4-bit";
      return true;
    }
    SD_MMC.end();
  }
  SD_MMC.setPins(clk, cmd, d0);
  if (SD_MMC.begin(kMount, true, false, 20000)) {
    g_mounted = true;
    g_width = "1-bit";
    return true;
  }
  SD_MMC.end();
  return false;
}

bool format() {
  if (g_mounted) SD_MMC.end();
  g_mounted = false;
  const int clk = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  const int cmd = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  const int d0 = M5.getPin(m5::pin_name_t::sd_spi_miso);
  const int d3 = M5.getPin(m5::pin_name_t::sd_spi_cs);
  SD_MMC.setPins(clk, cmd, d0, d0 + 1, d0 + 2, d3);
  // format_if_mount_failed: a card that mounts is left alone, so a card
  // that is already FAT is not erased by this.
  if (SD_MMC.begin(kMount, false, true, 20000)) {
    g_mounted = true;
    g_width = "4-bit";
  }
  return g_mounted;
}

bool mounted() { return g_mounted; }
fs::FS &fs() { return SD_MMC; }
const char *mountPoint() { return kMount; }
uint64_t cardMB() {
  return g_mounted ? SD_MMC.cardSize() / (1024ULL * 1024ULL) : 0;
}
uint64_t freeBytes() {
  return g_mounted ? SD_MMC.totalBytes() - SD_MMC.usedBytes() : 0;
}
const char *busWidth() { return g_width; }

}  // namespace sdcard
}  // namespace burner
