#include "runner.h"

#include <esp_heap_caps.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>

#include "../core/usb_esp.h"

extern "C" int minipro_main(int argc, char **argv);
extern "C" int optind, opterr;

namespace burner {
namespace runner {
namespace {

constexpr int kMaxLines = 200;

SemaphoreHandle_t g_mux;
std::deque<std::string> g_lines;
Status g_status;
std::vector<std::string> g_args;
volatile bool g_go = false;
volatile bool g_want_pause = false;
volatile bool g_paused = false;
uint32_t g_t0 = 0;

// ---- minipro's stdout and stderr -------------------------------------------
//
// minipro redraws its progress line in place with "\r\e[K<label> NN%". A
// carriage return starts the line over, escapes are dropped, and a line with
// no newline yet is the current phase, whose trailing "NN%" is the progress.

char g_out[160];
int g_out_len = 0;
bool g_escape = false;

void pushLine(const std::string &s) {
  Serial.println(s.c_str());
  g_lines.push_back(s);
  while ((int)g_lines.size() > kMaxLines) g_lines.pop_front();
  g_status.seq++;
}

void updatePhase() {
  std::string text(g_out, g_out_len);
  int pct = -1;
  const size_t p = text.rfind('%');
  if (p != std::string::npos && p == text.size() - 1) {
    size_t d = p;
    while (d > 0 && isdigit((unsigned char)text[d - 1])) d--;
    if (d < p) {
      pct = atoi(text.c_str() + d);
      text.erase(d);
    }
  }
  while (!text.empty() && text.back() == ' ') text.pop_back();
  g_status.phase = text;
  g_status.percent = pct;
  g_status.seq++;
}

int outWrite(void *, const char *buf, int n) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  for (int i = 0; i < n; i++) {
    const char c = buf[i];
    if (g_escape) {
      if (isalpha((unsigned char)c)) g_escape = false;
      continue;
    }
    if (c == 0x1B) {
      g_escape = true;
    } else if (c == '\n') {
      pushLine(std::string(g_out, g_out_len));
      g_out_len = 0;
      g_status.phase.clear();
      g_status.percent = -1;
    } else if (c == '\r') {
      g_out_len = 0;
    } else if (g_out_len < (int)sizeof(g_out) - 1) {
      g_out[g_out_len++] = c;
    }
  }
  if (g_out_len) updatePhase();
  xSemaphoreGive(g_mux);
  return n;
}

void task(void *) {
  // This task's stdio is minipro's; point it at the capture.
  FILE *f = fwopen(nullptr, outWrite);
  setvbuf(f, nullptr, _IONBF, 0);
  __getreent()->_stdout = f;
  __getreent()->_stderr = f;
  for (;;) {
    if (!g_go) {
      delay(20);
      continue;
    }
    // Charging off first, and give the rail a moment to come up.
    g_want_pause = true;
    for (int i = 0; i < 100 && !g_paused; i++) delay(10);
    delay(150);

    std::vector<char *> argv;
    argv.push_back((char *)"minipro");
    for (auto &a : g_args) argv.push_back((char *)a.c_str());
    argv.push_back(nullptr);
    usbdev::resetStats();
    optind = 0;   // newlib: a full getopt reset between runs
    opterr = 1;
    const int rc = minipro_main((int)argv.size() - 1, argv.data());
    fflush(stdout);
    fflush(stderr);

    xSemaphoreTake(g_mux, portMAX_DELAY);
    if (g_out_len) {
      pushLine(std::string(g_out, g_out_len));
      g_out_len = 0;
    }
    g_status.rc = rc;
    g_status.busy = false;
    g_status.finished = true;
    g_status.phase.clear();
    g_status.percent = -1;
    g_status.elapsed_ms = millis() - g_t0;
    g_status.seq++;
    xSemaphoreGive(g_mux);
    g_want_pause = false;
    g_go = false;
    // Where the time went, to serial only: USB time is submit-to-completion,
    // the rest is minipro, the SD card and this firmware.
    {
      static const char *names[4] = {"cmd out", "cmd in", "data out", "data in"};
      uint64_t usb_us = 0;
      for (int i = 0; i < 4; i++) {
        const auto st = usbdev::stats(i);
        usb_us += st.us;
        if (st.count)
          Serial.printf("usb %-8s %6lu transfers, avg %5lu us, max %6lu us\n", names[i], (unsigned long)st.count,
                        (unsigned long)(st.us / st.count), (unsigned long)st.max_us);
      }
      const uint32_t ms = millis() - g_t0;
      Serial.printf("job %lu ms: usb %lu ms, other %lu ms\n", (unsigned long)ms, (unsigned long)(usb_us / 1000),
                    (unsigned long)(ms - usb_us / 1000));
    }
    Serial.printf("== done %d\n", rc);
  }
}

}  // namespace

void begin() {
  g_mux = xSemaphoreCreateMutex();
  // minipro's main() keeps PATH_MAX buffers and the like on the stack. The
  // stack is in PSRAM: internal RAM is what the Wi-Fi transport runs out of
  // (uploads stalled for good with 4 KB of DMA memory left in one piece).
  xTaskCreatePinnedToCoreWithCaps(task, "minipro", 48 * 1024, nullptr, 3, nullptr, 1, MALLOC_CAP_SPIRAM);
}

bool start(const std::vector<std::string> &args, const char *title) {
  if (g_go) return false;
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_args = args;
  g_status.busy = true;
  g_status.title = title;
  g_status.phase = "Starting...";
  g_status.percent = -1;
  g_status.rc = 0;
  g_status.seq++;
  g_t0 = millis();
  std::string cmd = "minipro";
  for (auto &a : args) cmd += " " + a;
  pushLine("> " + cmd);
  xSemaphoreGive(g_mux);
  g_go = true;
  return true;
}

bool busy() { return g_go; }

Status status() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  Status s = g_status;
  if (s.busy) s.elapsed_ms = millis() - g_t0;
  xSemaphoreGive(g_mux);
  return s;
}

std::vector<std::string> lines(int max) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const int n = (int)g_lines.size();
  const int first = n > max ? n - max : 0;
  std::vector<std::string> out(g_lines.begin() + first, g_lines.end());
  xSemaphoreGive(g_mux);
  return out;
}

void clearLog() {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_lines.clear();
  g_status.seq++;
  xSemaphoreGive(g_mux);
}

void note(const char *fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  xSemaphoreTake(g_mux, portMAX_DELAY);
  pushLine(buf);
  xSemaphoreGive(g_mux);
}

bool wantsChargePaused() { return g_want_pause; }
void setChargePaused(bool paused) { g_paused = paused; }

}  // namespace runner
}  // namespace burner
