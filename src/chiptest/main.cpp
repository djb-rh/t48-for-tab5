// Phase 1 chip test: minipro itself, running on the Tab5 against the T48.
//
// minipro's main.c is compiled in unchanged (minipro_main.c renames its
// main) and driven with an argv, so every read, write, verify and blank
// check goes through exactly the code the Mac runs. Only usb_nix.c is
// replaced (usb_esp.cpp). Its database lives on the flash filesystem at /fs
// (data/, cut down by tools/trim_infoic.py) until the SD card takes over.
//
// Serial commands, one per line:
//   m <minipro arguments>   e.g. m -p TMS27C512@DIP28 -r /fs/read.bin
//   ls                      list /fs
//   crc <file>              size, CRC32 and byte sum
//   get <file>              hex dump between BEGIN/END lines (tools/mp.py get)
//   rm <file>
// Every command ends with a line "== done <rc>".

#include <LittleFS.h>
#include <M5Unified.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "usb_esp.h"

extern "C" int minipro_main(int argc, char **argv);
extern "C" int optind, opterr;

namespace {

// ---- log: serial and the glass ----------------------------------------------

constexpr int kLines = 25;
constexpr int kCols = 100;
char g_lines[kLines][kCols];
int g_head = 0, g_count = 0;
char g_progress[kCols] = "";
char g_status[kCols] = "";
bool g_dirty = true;
SemaphoreHandle_t g_log_mux;

void logLine(const char *s) {
  Serial.println(s);
  xSemaphoreTake(g_log_mux, portMAX_DELAY);
  strlcpy(g_lines[g_head], s, kCols);
  g_head = (g_head + 1) % kLines;
  if (g_count < kLines) g_count++;
  g_progress[0] = 0;
  g_dirty = true;
  xSemaphoreGive(g_log_mux);
}

void logf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  logLine(buf);
}

void drawLog() {
  xSemaphoreTake(g_log_mux, portMAX_DELAY);
  if (!g_dirty) {
    xSemaphoreGive(g_log_mux);
    return;
  }
  g_dirty = false;
  auto &d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setCursor(8, 8);
  d.print("tab5-burner chip test   ");
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.print(g_status);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  const int first = (g_head - g_count + kLines) % kLines;
  for (int i = 0; i < g_count; i++) {
    d.setCursor(8, 40 + i * 26);
    d.print(g_lines[(first + i) % kLines]);
  }
  if (g_progress[0]) {
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.setCursor(8, 40 + g_count * 26);
    d.print(g_progress);
  }
  d.endWrite();
  xSemaphoreGive(g_log_mux);
}

// ---- minipro's stdout/stderr ---------------------------------------------------
//
// minipro redraws its progress line with "\r\e[K"; a carriage return starts
// the line again and escape sequences are dropped. A finished line is logged;
// an unfinished one is shown as progress.

char g_out[kCols];
int g_out_len = 0;
bool g_in_escape = false;

void outFlushProgress() {
  xSemaphoreTake(g_log_mux, portMAX_DELAY);
  memcpy(g_progress, g_out, g_out_len);
  g_progress[g_out_len] = 0;
  g_dirty = true;
  xSemaphoreGive(g_log_mux);
}

int outWrite(void *, const char *buf, int n) {
  for (int i = 0; i < n; i++) {
    const char c = buf[i];
    if (g_in_escape) {
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) g_in_escape = false;
      continue;
    }
    if (c == 0x1B) {
      g_in_escape = true;
    } else if (c == '\n') {
      g_out[g_out_len] = 0;
      logLine(g_out);
      g_out_len = 0;
    } else if (c == '\r') {
      g_out_len = 0;
    } else if (g_out_len < kCols - 1) {
      g_out[g_out_len++] = c;
    }
  }
  if (g_out_len) outFlushProgress();
  return n;
}

FILE *g_stream = nullptr;

// ---- commands ----------------------------------------------------------------

volatile bool g_busy = false;
volatile bool g_want_charge = true;   // loop() owns the charger (I2C)
volatile bool g_charge_is = true;
char g_cmdline[256];

int splitArgs(char *s, char **argv, int max) {
  int argc = 0;
  while (*s && argc < max) {
    while (*s == ' ') s++;
    if (!*s) break;
    if (*s == '"') {
      argv[argc++] = ++s;
      while (*s && *s != '"') s++;
    } else {
      argv[argc++] = s;
      while (*s && *s != ' ') s++;
    }
    if (*s) *s++ = 0;
  }
  return argc;
}

int cmdMinipro(char *args) {
  char *argv[32];
  argv[0] = (char *)"minipro";
  const int argc = 1 + splitArgs(args, argv + 1, 30);
  argv[argc] = nullptr;
  // The T48 sags its supply while the battery charges (4.37 V against
  // 4.97 V): pause charging for the duration.
  g_want_charge = false;
  for (int i = 0; i < 100 && g_charge_is; i++) delay(10);
  delay(200);
  optind = 0;   // newlib: a full getopt reset
  opterr = 1;
  const uint32_t t0 = millis();
  const int rc = minipro_main(argc, argv);
  fflush(stdout);
  fflush(stderr);
  if (g_out_len) {   // a last line without its newline
    g_out[g_out_len] = 0;
    logLine(g_out);
    g_out_len = 0;
  }
  g_want_charge = true;
  logf("minipro exited %d after %.1f s", rc, (millis() - t0) / 1000.0f);
  return rc;
}

int cmdLs() {
  DIR *d = opendir("/fs");
  if (!d) return 1;
  while (dirent *e = readdir(d)) {
    char path[300];
    snprintf(path, sizeof(path), "/fs/%s", e->d_name);
    struct stat st;
    stat(path, &st);
    logf("%9ld  %s", (long)st.st_size, e->d_name);
  }
  closedir(d);
  logf("free %u KB", (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
  return 0;
}

uint8_t *loadFile(const char *path, size_t *size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    logf("%s: cannot open", path);
    return nullptr;
  }
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *buf = (uint8_t *)ps_malloc(*size ? *size : 1);
  if (buf && fread(buf, 1, *size, f) != *size) {
    free(buf);
    buf = nullptr;
  }
  fclose(f);
  return buf;
}

int cmdCrc(const char *path) {
  size_t n = 0;
  uint8_t *b = loadFile(path, &n);
  if (!b) return 1;
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += b[i];
  logf("%s: %u bytes  crc32 %08lx  sum16 %04lx", path, (unsigned)n,
       (unsigned long)esp_rom_crc32_le(0, b, n), (unsigned long)(sum & 0xFFFF));
  free(b);
  return 0;
}

// Straight to serial, not the log: 64 KB of hex would bury the screen.
int cmdGet(const char *path) {
  size_t n = 0;
  uint8_t *b = loadFile(path, &n);
  if (!b) return 1;
  Serial.setTxTimeoutMs(1000);   // the host is reading; do not drop bytes
  Serial.printf("BEGIN %u\n", (unsigned)n);
  char line[140];
  for (size_t i = 0; i < n; i += 64) {
    int o = 0;
    for (size_t k = i; k < i + 64 && k < n; k++) o += sprintf(line + o, "%02x", b[k]);
    line[o++] = '\n';
    Serial.write((const uint8_t *)line, o);
  }
  Serial.printf("END %08lx\n", (unsigned long)esp_rom_crc32_le(0, b, n));
  Serial.flush();
  Serial.setTxTimeoutMs(0);
  logf("sent %s (%u bytes)", path, (unsigned)n);
  free(b);
  return 0;
}

void runnerTask(void *) {
  // This task's stdio goes to the log, so minipro's fprintf(stderr, ...)
  // ends up on the glass and the serial port.
  g_stream = fwopen(nullptr, outWrite);
  setvbuf(g_stream, nullptr, _IONBF, 0);
  __getreent()->_stdout = g_stream;
  __getreent()->_stderr = g_stream;
  for (;;) {
    if (!g_busy) {
      delay(20);
      continue;
    }
    char *line = g_cmdline;
    int rc = 1;
    if (!strncmp(line, "m ", 2)) rc = cmdMinipro(line + 2);
    else if (!strcmp(line, "ls")) rc = cmdLs();
    else if (!strncmp(line, "crc ", 4)) rc = cmdCrc(line + 4);
    else if (!strncmp(line, "get ", 4)) rc = cmdGet(line + 4);
    else if (!strncmp(line, "rm ", 3)) rc = remove(line + 3) == 0 ? 0 : 1;
    else logf("unknown command: %s", line);
    Serial.printf("== done %d\n", rc);
    g_busy = false;
  }
}

}  // namespace

// A tap runs this, so a test can be done on battery with no computer
// attached (the serial port goes away with the USB-C cable).
constexpr const char *kTapCommand = "m -p TMS27C512@DIP28 -w /fs/read.bin";

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  g_log_mux = xSemaphoreCreateMutex();
  M5.Display.setRotation(3);
  M5.Display.setBrightness(180);
  M5.Display.setFont(&fonts::FreeMono12pt7b);

  if (!LittleFS.begin(true, "/fs")) logf("LittleFS mount failed");
  else logf("fs: %u KB used of %u KB", (unsigned)(LittleFS.usedBytes() / 1024),
            (unsigned)(LittleFS.totalBytes() / 1024));

  // Charging off while the T48 powers up: its inrush on top of ~700 mA of
  // charging browned the Tab5 out when it ran from a laptop port. loop()
  // turns charging back on once things have settled.
  M5.Power.setBatteryCharge(false);
  g_charge_is = false;
  g_want_charge = false;
  delay(100);
  M5.Power.setExtOutput(true, m5::ext_USB);
  usbdev::begin(logLine);
  delay(200);
  usbdev::powerPort(true);

  // minipro's main() wants a big stack (PATH_MAX buffers and the like).
  xTaskCreatePinnedToCore(runnerTask, "minipro", 48 * 1024, nullptr, 3, nullptr, 1);
  // Give the programmer a moment to enumerate before charging resumes.
  for (int i = 0; i < 300 && !usbdev::attached(); i++) delay(10);
  delay(500);
  g_want_charge = true;
  logf("ready: m <minipro args> | ls | crc <f> | get <f> | rm <f>");
  logf("tap the screen to run: %s", kTapCommand);
}

void loop() {
  M5.update();
  if (M5.Touch.getDetail().wasPressed() && !g_busy) {
    strlcpy(g_cmdline, kTapCommand, sizeof(g_cmdline));
    logf("> (tap) %s", kTapCommand);
    g_busy = true;
  }
  static char buf[256];
  static int len = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len && !g_busy) {
        buf[len] = 0;
        strlcpy(g_cmdline, buf, sizeof(g_cmdline));
        logf("> %s", buf);
        g_busy = true;
      }
      len = 0;
    } else if (len < (int)sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
  if (g_want_charge != g_charge_is) {
    M5.Power.setBatteryCharge(g_want_charge);
    g_charge_is = g_want_charge;
  }
  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    snprintf(g_status, sizeof(g_status), "%s  %s  bat %d mA", usbdev::attached() ? "T48" : "no T48",
             g_charge_is ? "charging" : "charge paused", (int)M5.Power.getBatteryCurrent());
    xSemaphoreTake(g_log_mux, portMAX_DELAY);
    g_dirty = true;
    xSemaphoreGive(g_log_mux);
  }
  drawLog();
  delay(20);
}
