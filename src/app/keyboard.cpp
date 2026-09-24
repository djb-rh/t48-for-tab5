#include "keyboard.h"

#include <M5Unified.h>
#include <driver/gpio.h>

#include <cstdio>
#include <cstring>

namespace burner {
namespace keyboard {
namespace {

constexpr uint8_t kAddress = 0x6D;
constexpr int kSda = 0, kScl = 1, kInt = 50;
constexpr uint32_t kFreq = 400000;

constexpr uint8_t kRegIntCfg = 0x00;
constexpr uint8_t kRegEventNum = 0x02;
constexpr uint8_t kRegMode = 0x10;
constexpr uint8_t kRegHidEvent = 0x30;  // modifier, then usage code
constexpr uint8_t kRegFirmware = 0xFE;
constexpr uint8_t kModeHid = 1;

constexpr uint32_t kProbeMs = 700;   // while absent, how often to look
constexpr uint32_t kCheckMs = 3000;  // while present, how often to make sure
constexpr uint32_t kPollMs = 15;     // while present, how often to ask for keys

bool g_bus = false;
bool g_present = false;
uint32_t g_next_probe = 0;
uint32_t g_next_poll = 0;

// A record of everything decoded, read back over serial: the keyboard is
// typed on whenever the owner gets to it, not while a listener is running.
char g_log[512];
int g_log_len = 0;
uint32_t g_polls = 0, g_nonzero = 0;
void note(const char *text) {
  const int n = (int)strlen(text);
  if (g_log_len + n + 1 >= (int)sizeof(g_log)) g_log_len = 0;
  memcpy(g_log + g_log_len, text, n);
  g_log_len += n;
  g_log[g_log_len] = 0;
}

constexpr int kQueue = 32;
Key g_queue[kQueue];
int g_head = 0, g_tail = 0;

// The keyboard is on its own pins, and the chip has no third I2C port to
// give them: the Grove port's bus is moved over. Nothing here uses Grove.
bool bus() {
  if (g_bus) return true;
  M5.Ex_I2C.release();
  g_bus = M5.Ex_I2C.begin(I2C_NUM_0, kSda, kScl);
  if (g_bus) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << kInt;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
  }
  return g_bus;
}

bool answers() {
  // A bare address probe: start, then stop. The library's readRegister8
  // returns a byte whether or not anyone was there.
  const bool ok = M5.Ex_I2C.start(kAddress, false, kFreq) && M5.Ex_I2C.write(kRegFirmware);
  M5.Ex_I2C.stop();
  return ok;
}

void push(const Key &k) {
  const int next = (g_head + 1) % kQueue;
  if (next == g_tail) return;
  g_queue[g_head] = k;
  g_head = next;
}

// USB HID usage codes (page 7) on the US layout the keyboard carries.
char usageToChar(uint8_t code, bool shift) {
  if (code >= 0x04 && code <= 0x1D) {
    const char c = (char)('a' + (code - 0x04));
    return shift ? (char)(c - 32) : c;
  }
  if (code >= 0x1E && code <= 0x27) {
    static const char plain[] = "1234567890", shifted[] = "!@#$%^&*()";
    return shift ? shifted[code - 0x1E] : plain[code - 0x1E];
  }
  struct Pair { uint8_t code; char plain, shifted; };
  static const Pair kPairs[] = {
      {0x2C, ' ', ' '},  {0x2D, '-', '_'},  {0x2E, '=', '+'},  {0x2F, '[', '{'},
      {0x30, ']', '}'},  {0x31, '\\', '|'}, {0x33, ';', ':'},  {0x34, '\'', '"'},
      {0x35, '`', '~'},  {0x36, ',', '<'},  {0x37, '.', '>'},  {0x38, '/', '?'},
  };
  for (const Pair &p : kPairs) {
    if (p.code == code) return shift ? p.shifted : p.plain;
  }
  return 0;
}

Special usageToSpecial(uint8_t code) {
  switch (code) {
    case 0x28: return Special::Enter;
    case 0x2A: return Special::Backspace;
    case 0x29: return Special::Escape;
    case 0x2B: return Special::Tab;
    case 0x4C: return Special::Delete;
    case 0x4F: return Special::Right;
    case 0x50: return Special::Left;
    case 0x51: return Special::Down;
    case 0x52: return Special::Up;
    case 0x4A: return Special::Home;
    case 0x4B: return Special::PageUp;
    case 0x4D: return Special::End;
    case 0x4E: return Special::PageDown;
    default: return Special::None;
  }
}

void found() {
  g_present = true;
  M5.Ex_I2C.writeRegister8(kAddress, kRegMode, kModeHid, kFreq);
  M5.Ex_I2C.writeRegister8(kAddress, kRegEventNum, 0, kFreq);  // drop what queued unseen
  M5.Ex_I2C.writeRegister8(kAddress, kRegIntCfg, 0x07, kFreq);
  const uint8_t ver = M5.Ex_I2C.readRegister8(kAddress, kRegFirmware, kFreq);
  const uint8_t mode = M5.Ex_I2C.readRegister8(kAddress, kRegMode, kFreq);
  Serial.printf("keyboard: Tab5 Keyboard found, firmware %u, mode %u, int line %d\n",
                (unsigned)ver, (unsigned)mode, gpio_get_level((gpio_num_t)kInt));
}

void lost() {
  g_present = false;
  Serial.println("keyboard: gone");
}

void readEvents() {
  const uint8_t n = M5.Ex_I2C.readRegister8(kAddress, kRegEventNum, kFreq);
  g_polls++;
  if (n == 0 || n > 32) return;
  g_nonzero++;
  for (int i = 0; i < n; i++) {
    uint8_t buf[2] = {0xFF, 0xFF};
    if (!M5.Ex_I2C.readRegister(kAddress, kRegHidEvent, buf, 2, kFreq)) return;
    if (buf[0] == 0xFF && buf[1] == 0xFF) return;  // the queue ran dry
    const uint8_t mod = buf[0], code = buf[1];
    if (code == 0) continue;  // a modifier alone, or a release
    Key k;
    const bool shift = (mod & 0x22) != 0;
    k.ctrl = (mod & 0x11) != 0;
    k.alt = (mod & 0x44) != 0;
    k.special = usageToSpecial(code);
    if (k.special == Special::None) k.ch = usageToChar(code, shift);
    if (k.ch == 0 && k.special == Special::None) continue;
    char text[24];
    if (k.ch) snprintf(text, sizeof(text), "%c", k.ch);
    else snprintf(text, sizeof(text), "<%d:%02X>", (int)k.special, code);
    note(text);
    push(k);
  }
}

}  // namespace

void begin() {
  g_next_probe = 0;
  g_next_poll = 0;
}

bool present() { return g_present; }

void dumpLog() {
  Serial.printf("keyboard: present=%d polls=%lu with_events=%lu typed=[%s]\n", (int)g_present,
                (unsigned long)g_polls, (unsigned long)g_nonzero, g_log);
}

void inject(const Key &k) { push(k); }

bool take(Key *out) {
  if (g_head == g_tail) return false;
  if (out) *out = g_queue[g_tail];
  g_tail = (g_tail + 1) % kQueue;
  return true;
}

void tick(uint32_t now_ms) {
  if (!g_present) {
    if ((int32_t)(now_ms - g_next_probe) < 0) return;
    g_next_probe = now_ms + kProbeMs;
    if (bus() && answers()) found();
    return;
  }
  // Present: read when the line says so, and every few milliseconds anyway,
  // since a level interrupt that was already low when we looked is easy to
  // miss without a handler.
  if ((int32_t)(now_ms - g_next_probe) >= 0) {
    g_next_probe = now_ms + kCheckMs;
    if (!answers()) {
      lost();
      g_next_probe = now_ms + kProbeMs;
      return;
    }
  }
  if ((int32_t)(now_ms - g_next_poll) < 0 && gpio_get_level((gpio_num_t)kInt) != 0) return;
  g_next_poll = now_ms + kPollMs;
  readEvents();
}

}  // namespace keyboard
}  // namespace burner
