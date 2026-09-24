// T48 for Tab5: an M5Stack Tab5 and its keyboard as a stand-alone front end
// for an XGecu T48 on the USB-A port.
//
// minipro does the programming (third_party/minipro, driven by runner.cpp);
// the part library, images and minipro's per-part database live on the SD
// card under /sdcard/burner; a browser file manager serves that folder over
// Wi-Fi (web.cpp); the serial console (console.cpp) is for tests.

#include <M5Unified.h>
#include <SD_MMC.h>
#include <esp_log.h>

#include "../core/usb_esp.h"
#include "app.h"
#include "console.h"
#include "keyboard.h"
#include "parts.h"
#include "runner.h"
#include "sdcard.h"
#include "ui.h"
#include "web.h"

using namespace burner;

namespace {

bool g_charge_on = true;
bool g_booted = false;

void usbLog(const char *line) { runner::note("%s", line); }

void splash(const char *msg) {
  auto &d = M5.Display;
  d.fillScreen(ui::kBg);
  ui::text(ui::W / 2, 300, "T48 for Tab5", ui::Font::Big, ui::kText, ui::kBg, 1);
  ui::text(ui::W / 2, 370, msg, ui::Font::Body, ui::kDim, ui::kBg, 1);
}

void setCharging(bool on) {
  if (on == g_charge_on) return;
  M5.Power.setBatteryCharge(on);
  g_charge_on = on;
}

void updateStatus() {
  auto &s = app::status();
  s.t48 = usbdev::attached();
  s.sd = sdcard::mounted();
  s.wifi = web::statusText();
  s.battery = M5.Power.getBatteryLevel();
  // CHG_STAT does not work on the Tab5; a positive current means charging.
  s.charging = g_charge_on && M5.Power.getBatteryCurrent() > 50;
  static uint32_t free_ms = 0;
  if (s.sd && (free_ms == 0 || millis() - free_ms > 30000)) {
    free_ms = millis();
    s.sd_free = sdcard::freeBytes();
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;   // every outgoing 5 V rail; only USB-A is wanted
  M5.begin(cfg);
  Serial.setRxBufferSize(16384);   // console 'put' streams 4 KB at a time
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);        // never block on a USB-serial nobody reads
  // ESP-IDF's own log lines (the Wi-Fi stack's) go to the same USB serial
  // port and are not covered by the timeout above; with the Mac attached but
  // nothing reading, they were the suspect in uploads killing the network.
  esp_log_level_set("*", ESP_LOG_NONE);
  M5.Display.setRotation(3);
  M5.Display.setBrightness(180);
  splash("Starting...");

  runner::begin();

  // Charging off while the T48 powers up: its inrush on top of ~700 mA of
  // charging browned the Tab5 out on a laptop port.
  setCharging(false);
  delay(100);
  M5.Power.setExtOutput(true, m5::ext_USB);
  usbdev::begin(usbLog);
  delay(200);
  usbdev::powerPort(true);

  splash("Reading the SD card...");
  if (!sdcard::begin()) {
    runner::note("No SD card: the part library and images live on it");
  } else {
    std::string why;
    if (!parts::load(&why)) runner::note("%s", why.c_str());
  }
  app::begin();
  keyboard::begin();
  web::begin();

  for (int i = 0; i < 200 && !usbdev::attached(); i++) delay(10);
  updateStatus();
  app::goMain();
  g_booted = true;
  Serial.println("ready");
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  keyboard::tick(now);
  console::poll();
  web::loop();

  std::string chosen;
  if (web::takeChosenImage(&chosen)) {
    app::setImage(chosen);
    runner::note("Image chosen from the browser: %s", app::baseName(chosen).c_str());
    app::goMain();
  }

  // The T48 sees 4.4 V instead of 5 V while the battery charges, so charging
  // pauses for every job.
  const bool pause = runner::wantsChargePaused();
  setCharging(g_booted && !pause);
  runner::setChargePaused(!g_charge_on);

  static uint32_t status_ms = 0;
  if (now - status_ms > 1000) {
    status_ms = now;
    updateStatus();
  }

  ui::pump();
  delay(5);
}
