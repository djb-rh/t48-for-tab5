#include "ui.h"

#include <cctype>
#include <cstring>

namespace burner {
namespace ui {
namespace {

Screen *g_screen = nullptr;
volatile bool g_tap_pending = false;
volatile int g_tap_x = 0, g_tap_y = 0;

// Touch: a press that moves more than a few pixels becomes a drag, and a
// drag never also counts as a tap.
bool g_down = false, g_dragging = false;
int g_start_y = 0, g_last_y = 0, g_start_x = 0;
constexpr int kDragSlop = 14;
bool g_on_keys = false;  // this press began on the on-screen keys

// The on-screen keys: 11 units across, five rows. Digits stay on both pages,
// since chip names are mostly digits.
constexpr int kKeyRowH = 60, kKeysH = 5 * kKeyRowH, kUnit = W / 11, kFlashMs = 90;
const char *const kLetters[4] = {"1234567890", "qwertyuiop-", "asdfghjkl.", "zxcvbnm@/"};
const char *const kSymbols[4] = {"1234567890", "!#$%^&*()_+", "[]{}|\\;:'\"", "=,?<>~`@/"};

enum class Act : uint8_t { Char, Del, Enter, Shift, Page, Space, Hide };
enum class Shift : uint8_t { Off, Once, Lock };

bool g_keys_open = false, g_symbols = false;
Shift g_shift = Shift::Off;
int g_lit = -1;          // the key flashing after a tap
uint32_t g_lit_ms = 0;

char keyChar(char c) {
  return !g_symbols && g_shift != Shift::Off ? (char)toupper((unsigned char)c) : c;
}

// Calls f(index, x, y, w, label, act, ch) for every key.
template <typename F>
void eachKey(F f) {
  const char *const *rows = g_symbols ? kSymbols : kLetters;
  const int top = H - kKeysH;
  int index = 0;
  for (int r = 0; r < 5; r++) {
    const int y = top + r * kKeyRowH;
    int x = (W - 11 * kUnit) / 2;
    auto put = [&](int units, const char *label, Act act, char ch) {
      f(index++, x, y, units * kUnit, label, act, ch);
      x += units * kUnit;
    };
    if (r == 3) put(2, "Shift", Act::Shift, 0);
    if (r == 4) {
      put(2, g_symbols ? "ABC" : "Sym", Act::Page, 0);
      put(7, "Space", Act::Space, ' ');
      put(2, "Hide", Act::Hide, 0);
      break;
    }
    for (const char *c = rows[r]; *c; c++) {
      const char one[2] = {keyChar(*c), 0};
      put(1, one, Act::Char, one[0]);
    }
    if (r == 0) put(1, "Del", Act::Del, 0);
    if (r == 2) put(1, "Enter", Act::Enter, 0);
  }
}

void drawKey(int index, int x, int y, int w, const char *label, Act act) {
  uint32_t fill = act == Act::Char || act == Act::Space ? kPanel2 : kPanel;
  uint32_t fg = kText;
  if (act == Act::Enter) fill = kAccentDim;
  if (act == Act::Shift && !g_symbols && g_shift == Shift::Once) fill = kAccentDim;
  if (act == Act::Shift && !g_symbols && g_shift == Shift::Lock) fill = kAccent;
  if (act == Act::Shift && g_symbols) fg = kFaint;
  if (index == g_lit) fill = kAccent;
  panel(x + 3, y + 3, w - 6, kKeyRowH - 6, fill);
  const Font f = strlen(label) > 1 ? Font::Small : Font::Body;
  text(x + w / 2, y + (kKeyRowH - fontHeight(f)) / 2, label, f, fg, fill, 1);
}

void drawKeys() {
  d().fillRect(0, H - kKeysH, W, kKeysH, kBg);
  d().drawFastHLine(0, H - kKeysH, W, kBorder);
  eachKey([](int i, int x, int y, int w, const char *label, Act act, char) { drawKey(i, x, y, w, label, act); });
}

void flashKey(int index) {
  const int old = g_lit;
  g_lit = index;
  g_lit_ms = millis();
  d().startWrite();
  eachKey([&](int i, int x, int y, int w, const char *label, Act act, char) {
    if (i == old || i == index) drawKey(i, x, y, w, label, act);
  });
  d().endWrite();
}

void keysTap(int tx, int ty) {
  int index = -1;
  Act act = Act::Char;
  char ch = 0;
  eachKey([&](int i, int x, int y, int w, const char *, Act a, char c) {
    if (tx >= x && tx < x + w && ty >= y && ty < y + kKeyRowH) {
      index = i;
      act = a;
      ch = c;
    }
  });
  if (index < 0) return;
  keyboard::Key k;
  switch (act) {
    case Act::Hide:
      g_keys_open = false;
      redraw();
      return;
    case Act::Page:
      g_symbols = !g_symbols;
      break;
    case Act::Shift:
      if (g_symbols) return;
      g_shift = g_shift == Shift::Off ? Shift::Once : g_shift == Shift::Once ? Shift::Lock : Shift::Off;
      break;
    case Act::Del: k.special = keyboard::Special::Backspace; break;
    case Act::Enter: k.special = keyboard::Special::Enter; break;
    case Act::Char:
    case Act::Space: k.ch = ch; break;
  }
  if (act == Act::Page || act == Act::Shift) {
    d().startWrite();
    drawKeys();
    d().endWrite();
    return;
  }
  keyboard::inject(k);
  if (act == Act::Char && g_shift == Shift::Once) {
    g_shift = Shift::Off;
    d().startWrite();
    drawKeys();
    d().endWrite();
  }
  flashKey(index);
}

const lgfx::IFont *fontOf(Font f) {
  switch (f) {
    case Font::Small: return &fonts::DejaVu18;
    case Font::Body: return &fonts::DejaVu24;
    case Font::Big: return &fonts::DejaVu40;
    case Font::Mono: return &fonts::FreeMonoBold12pt7b;
    case Font::MonoSmall: return &fonts::FreeMono9pt7b;
    case Font::Sign: return &fonts::FreeSansBold24pt7b;
  }
  return &fonts::DejaVu24;
}

}  // namespace

M5GFX &d() { return M5.Display; }

void setFont(Font f) { d().setFont(fontOf(f)); }

int textWidth(const char *s, Font f) {
  setFont(f);
  return d().textWidth(s);
}

int fontHeight(Font f) {
  setFont(f);
  return d().fontHeight();
}

void text(int x, int y, const std::string &s, Font f, uint32_t fg, uint32_t bg, int align, int pad) {
  setFont(f);
  d().setTextColor(fg, bg);
  d().setTextDatum(align == 0 ? top_left : align == 1 ? top_center : top_right);
  d().setTextPadding(pad);
  d().drawString(s.c_str(), x, y);
  d().setTextPadding(0);
  d().setTextDatum(top_left);
}

std::string fit(const std::string &s, Font f, int max_w) {
  if (textWidth(s.c_str(), f) <= max_w) return s;
  std::string t = s;
  while (!t.empty() && textWidth((t + "...").c_str(), f) > max_w) t.pop_back();
  return t + "...";
}

void panel(int x, int y, int w, int h, uint32_t fill, uint32_t border) {
  d().fillRoundRect(x, y, w, h, 10, fill);
  if (border != fill) d().drawRoundRect(x, y, w, h, 10, border);
}

void drawButton(const Button &b) {
  uint32_t fill = kPanel2, border = kBorder, fg = kText;
  if (!b.enabled) {
    fg = kFaint;
  } else if (b.toggle && b.on) {
    fill = kAccentDim;
    border = kAccent;
  } else if (b.danger) {
    border = 0x8E2B2B;
  }
  panel(b.x, b.y, b.w, b.h, fill, border);
  const int fh = fontHeight(Font::Body);
  const int lw = b.w - (b.key.empty() ? 16 : 44);
  text(b.x + (b.key.empty() ? b.w / 2 : 14 + lw / 2), b.y + (b.h - fh) / 2, fit(b.label, Font::Body, lw),
       Font::Body, fg, fill, 1);
  if (!b.key.empty()) {
    const uint32_t kc = b.enabled ? kDim : kFaint;
    text(b.x + b.w - 10, b.y + 6, b.key, Font::Small, kc, fill, 2);
  }
  if (b.toggle) {
    // A lamp in the bottom-right corner says on or off at a glance.
    d().fillCircle(b.x + b.w - 16, b.y + b.h - 16, 6, b.on ? kGood : kFaint);
  }
}

int hit(const std::vector<Button> &bs, int x, int y) {
  for (const auto &b : bs)
    if (b.enabled && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return b.id;
  return -1;
}

void progressBar(int x, int y, int w, int h, int percent, uint32_t color) {
  d().fillRoundRect(x, y, w, h, h / 2, kPanel2);
  if (percent > 0) {
    const int fw = std::max(h, w * std::min(percent, 100) / 100);
    d().fillRoundRect(x, y, fw, h, h / 2, color);
  }
}

void show(Screen *s) {
  g_screen = s;
  redraw();
}

Screen *current() { return g_screen; }

void redraw() {
  if (!g_screen) return;
  d().startWrite();
  d().fillScreen(kBg);
  g_screen->draw();
  if (keysUp()) drawKeys();
  d().endWrite();
}

void openKeys(bool open, bool capsLock) {
  g_keys_open = open;
  if (open) {
    g_symbols = false;
    g_shift = capsLock ? Shift::Lock : Shift::Off;
  }
}

bool keysUp() { return g_keys_open && g_screen && g_screen->wantsKeys() && !keyboard::present(); }

int keysTop() { return keysUp() ? H - kKeysH : H; }

void injectTap(int x, int y) {
  g_tap_x = x;
  g_tap_y = y;
  g_tap_pending = true;
}

void pump() {
  if (!g_screen) return;
  Screen *s = g_screen;

  // Clipping the keyboard on or off moves the on-screen keys.
  static bool had_keyboard = keyboard::present();
  if (keyboard::present() != had_keyboard) {
    had_keyboard = !had_keyboard;
    if (s->wantsKeys()) redraw();
  }
  if (g_lit >= 0 && millis() - g_lit_ms > kFlashMs) {
    if (keysUp()) flashKey(-1);
    g_lit = -1;
  }

  keyboard::Key k;
  while (keyboard::take(&k)) {
    s->key(k);
    if (g_screen != s) return;   // the key changed screens
  }

  if (g_tap_pending) {
    g_tap_pending = false;
    if (keysUp() && g_tap_y >= keysTop()) keysTap(g_tap_x, g_tap_y);
    else s->tap(g_tap_x, g_tap_y);
    if (g_screen != s) return;
  }

  auto t = M5.Touch.getDetail();
  if (t.isPressed()) {
    if (!g_down) {
      g_down = true;
      g_dragging = false;
      g_start_x = t.x;
      g_start_y = g_last_y = t.y;
      g_on_keys = keysUp() && t.y >= keysTop();
    } else if (!g_on_keys) {
      if (!g_dragging && abs(t.y - g_start_y) > kDragSlop) g_dragging = true;
      if (g_dragging && t.y != g_last_y) {
        s->drag(t.y - g_last_y);
        g_last_y = t.y;
      }
    }
  } else if (g_down) {
    g_down = false;
    if (g_on_keys) keysTap(g_start_x, g_start_y);
    else if (!g_dragging) s->tap(g_start_x, g_start_y);
  }
  if (g_screen == s) s->tick();
}

}  // namespace ui
}  // namespace burner
