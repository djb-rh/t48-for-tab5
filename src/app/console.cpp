// The serial test console (tools/tab5.py drives it). One command a line;
// each ends with "== done <rc>" (a minipro run prints it when it finishes).
//
//   m <minipro args>      run minipro directly
//   ls <dir>  crc <file>  rm <file>  mkdir <dir>
//   get <file>            hex lines between BEGIN <size> and END <crc32>
//   put <file> <size>     'put: ready', then <size> raw bytes; ACK (0x06) per 4 KB
//   shot                  SHOT <w> <h>, raw RGB565 rows, ENDSHOT
//   tap <x> <y>           a touch
//   key <text>            typed keys; {enter} {esc} {tab} {bs} {del} {up}
//                         {down} {left} {right} {pgup} {pgdn} {home} {end},
//                         {^x} for Ctrl+x
//   wifi <ssid> [pass]   join and remember a network; 'wifi forget' clears it
//   memlog [off]         print memory every 2 s
//   mem                   free internal / DMA / PSRAM memory
//   sdformat ERASE        format the card (FAT); erases it
//   reboot

#include "console.h"

#include <esp_heap_caps.h>
#include <M5Unified.h>
#include <dirent.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <sys/stat.h>

#include <cstring>
#include <string>
#include <vector>

#include "keyboard.h"
#include "runner.h"
#include "sdcard.h"
#include "web.h"
#include "ui.h"

namespace burner {
namespace console {
namespace {

std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : s) {
    if (c == '"') quoted = !quoted;
    else if (c == ' ' && !quoted) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

void done(int rc) { Serial.printf("== done %d\n", rc); }

int cmdLs(const std::string &dir) {
  DIR *d = opendir(dir.c_str());
  if (!d) return 1;
  while (dirent *e = readdir(d)) {
    struct stat st;
    stat((dir + "/" + e->d_name).c_str(), &st);
    Serial.printf("%10ld %s%s\n", (long)st.st_size, e->d_name, S_ISDIR(st.st_mode) ? "/" : "");
  }
  closedir(d);
  return 0;
}

int cmdCrc(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return 1;
  static uint8_t *buf = (uint8_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
  uint32_t crc = 0, size = 0;
  size_t n;
  while ((n = fread(buf, 1, 16384, f)) > 0) {
    crc = esp_rom_crc32_le(crc, buf, n);
    size += n;
  }
  fclose(f);
  Serial.printf("%s: %lu bytes crc32 %08lx\n", path.c_str(), (unsigned long)size, (unsigned long)crc);
  return 0;
}

int cmdGet(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  Serial.setTxTimeoutMs(1000);
  Serial.printf("BEGIN %ld\n", size);
  uint8_t in[64];
  char line[140];
  uint32_t crc = 0;
  size_t n;
  while ((n = fread(in, 1, sizeof(in), f)) > 0) {
    crc = esp_rom_crc32_le(crc, in, n);
    int o = 0;
    for (size_t k = 0; k < n; k++) o += sprintf(line + o, "%02x", in[k]);
    line[o++] = '\n';
    Serial.write((const uint8_t *)line, o);
  }
  fclose(f);
  Serial.printf("END %08lx\n", (unsigned long)crc);
  Serial.flush();
  Serial.setTxTimeoutMs(0);
  return 0;
}

// Raw bytes after the command line, acknowledged in 4 KB steps so the link
// never outruns the card or the receive buffer.
int cmdPut(const std::string &path, long size) {
  // Make the folders on the way.
  for (size_t p = path.find('/', 1); p != std::string::npos; p = path.find('/', p + 1))
    mkdir(path.substr(0, p).c_str(), 0777);
  const std::string tmp = path + ".part";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return 1;
  static uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  long left = size;
  uint32_t crc = 0;
  Serial.setTxTimeoutMs(1000);
  Serial.println("put: ready");
  while (left > 0) {
    const int want = left < 4096 ? (int)left : 4096;
    int got = 0;
    const uint32_t t0 = millis();
    while (got < want && millis() - t0 < 5000) {
      const int a = Serial.available();
      if (a > 0) got += Serial.readBytes(buf + got, std::min(a, want - got));
      else delay(1);
    }
    if (got < want || fwrite(buf, 1, got, f) != (size_t)got) {
      fclose(f);
      remove(tmp.c_str());
      Serial.setTxTimeoutMs(0);
      Serial.printf("\nput: failed with %ld bytes left\n", left);
      return 1;
    }
    crc = esp_rom_crc32_le(crc, buf, got);
    left -= got;
    Serial.write(0x06);   // ACK: a byte no log line contains
  }
  fclose(f);
  remove(path.c_str());
  rename(tmp.c_str(), path.c_str());
  Serial.printf("\nput: %s %ld bytes crc32 %08lx\n", path.c_str(), size, (unsigned long)crc);
  Serial.setTxTimeoutMs(0);
  return 0;
}

// What is on the glass, read back from the DSI framebuffer and un-rotated
// (the same mapping as Panel_FrameBufferBase::drawPixelPreclipped).
int cmdShot() {
  auto *panel = (lgfx::Panel_DSI *)M5.Display.getPanel();
  const uint8_t *fb = (const uint8_t *)panel->config_detail().buffer;
  if (!fb) return 1;
  const size_t stride = ((size_t)panel->config().panel_width * 2 + 3) & ~(size_t)3;
  const uint8_t rot = (uint8_t)M5.Display.getRotation();
  const int pw = panel->config().panel_width, ph = panel->config().panel_height;
  const int w = (rot & 1) ? ph : pw, h = (rot & 1) ? pw : ph;
  // The host is reading; a dropped byte ruins the picture, so wait long, and
  // keep ESP-IDF's own log lines (the Wi-Fi stack's) out of the stream.
  esp_log_level_set("*", ESP_LOG_NONE);
  Serial.setTxTimeoutMs(10000);
  Serial.printf("SHOT %d %d\n", w, h);
  static uint16_t *row = (uint16_t *)heap_caps_malloc(1280 * 2, MALLOC_CAP_SPIRAM);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      size_t prow, pcol;
      switch (rot) {
        case 0: prow = y; pcol = x; break;
        case 1: prow = x; pcol = pw - 1 - y; break;
        case 2: prow = ph - 1 - y; pcol = pw - 1 - x; break;
        default: prow = ph - 1 - x; pcol = y; break;
      }
      row[x] = *(const uint16_t *)(fb + prow * stride + pcol * 2);
    }
    Serial.write((const uint8_t *)row, (size_t)w * 2);
  }
  Serial.flush();
  Serial.println("ENDSHOT");
  Serial.setTxTimeoutMs(0);
  esp_log_level_set("*", ESP_LOG_NONE);
  return 0;
}

int cmdKey(const std::string &text) {
  using keyboard::Special;
  static const struct { const char *name; Special s; } kNames[] = {
      {"enter", Special::Enter}, {"esc", Special::Escape}, {"tab", Special::Tab},
      {"bs", Special::Backspace}, {"del", Special::Delete}, {"up", Special::Up},
      {"down", Special::Down}, {"left", Special::Left}, {"right", Special::Right},
      {"pgup", Special::PageUp}, {"pgdn", Special::PageDown}, {"home", Special::Home},
      {"end", Special::End},
  };
  for (size_t i = 0; i < text.size(); i++) {
    keyboard::Key k;
    if (text[i] == '{') {
      const size_t e = text.find('}', i);
      if (e == std::string::npos) return 1;
      const std::string name = text.substr(i + 1, e - i - 1);
      i = e;
      if (name.size() == 2 && name[0] == '^') {
        k.ch = name[1];
        k.ctrl = true;
      } else {
        bool found = false;
        for (auto &n : kNames)
          if (name == n.name) {
            k.special = n.s;
            found = true;
          }
        if (!found) return 1;
      }
    } else {
      k.ch = text[i];
    }
    keyboard::inject(k);
  }
  return 0;
}

std::string g_line;
bool g_memlog = false;

void memLine() {
  Serial.printf("mem: internal %u KB free, largest %u KB; dma %u KB free, largest %u KB; wifi %d\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024), (int)web::state());
}

void run(const std::string &line) {
  const auto a = split(line);
  if (a.empty()) return;
  const std::string &c = a[0];
  if (c == "m") {
    std::vector<std::string> args(a.begin() + 1, a.end());
    if (!runner::start(args, "Console")) done(1);
    return;   // the runner prints "== done" when minipro returns
  }
  if (c == "ls") return done(cmdLs(a.size() > 1 ? a[1] : "/sdcard/burner"));
  if (c == "crc" && a.size() > 1) return done(cmdCrc(a[1]));
  if (c == "get" && a.size() > 1) return done(cmdGet(a[1]));
  if (c == "put" && a.size() > 2) return done(cmdPut(a[1], atol(a[2].c_str())));
  if (c == "rm" && a.size() > 1) return done(remove(a[1].c_str()) == 0 ? 0 : 1);
  if (c == "mkdir" && a.size() > 1) return done(mkdir(a[1].c_str(), 0777) == 0 ? 0 : 1);
  if (c == "shot") return done(cmdShot());
  if (c == "tap" && a.size() > 2) {
    ui::injectTap(atoi(a[1].c_str()), atoi(a[2].c_str()));
    return done(0);
  }
  if (c == "key" && line.size() > 4) return done(cmdKey(line.substr(4)));
  if (c == "sdformat") {
    // Deliberately awkward: it erases the card.
    if (a.size() < 2 || a[1] != "ERASE") {
      Serial.println("sdformat erases the card; say 'sdformat ERASE'");
      return done(1);
    }
    const bool ok = sdcard::format();
    Serial.printf("sdformat: %s, %llu MB\n", ok ? "mounted" : "FAILED", (unsigned long long)sdcard::cardMB());
    return done(ok ? 0 : 1);
  }
  if (c == "wifi" && a.size() > 1) {
    // Tests only; the real way is the setup hotspot or the N screen.
    if (a[1] == "forget") web::forget();
    else web::join(a[1], a.size() > 2 ? a[2] : "");
    return done(0);
  }
  if (c == "memlog") {
    g_memlog = a.size() < 2 || a[1] != "off";
    return done(0);
  }
  if (c == "mem") {
    // Internal (DMA-capable) RAM is what the Wi-Fi transport runs short of:
    // with 4 KB left in one piece, uploads stalled and the network died.
    Serial.printf("internal %u KB free, largest %u KB; dma %u KB free, largest %u KB; psram %u KB free\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return done(0);
  }
  if (c == "reboot") {
    done(0);
    Serial.flush();
    delay(100);
    ESP.restart();
  }
  Serial.printf("unknown command: %s\n", line.c_str());
  done(1);
}

}  // namespace

void poll() {
  static uint32_t mem_ms = 0;
  if (g_memlog && millis() - mem_ms > 2000) {
    mem_ms = millis();
    memLine();
  }
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (!g_line.empty()) {
        const std::string l = g_line;
        g_line.clear();
        run(l);
        return;   // a put reads its own bytes; start fresh next loop
      }
    } else if (g_line.size() < 400) {
      g_line += ch;
    }
  }
}

}  // namespace console
}  // namespace burner
