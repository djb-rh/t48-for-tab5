#include "ui.h"

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

const lgfx::IFont *fontOf(Font f) {
  switch (f) {
    case Font::Small: return &fonts::DejaVu18;
    case Font::Body: return &fonts::DejaVu24;
    case Font::Big: return &fonts::DejaVu40;
    case Font::Mono: return &fonts::FreeMonoBold12pt7b;
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

void text(int x, int y, const std::string &s, Font f, uint32_t fg, uint32_t bg, int align) {
  setFont(f);
  d().setTextColor(fg, bg);
  d().setTextDatum(align == 0 ? top_left : align == 1 ? top_center : top_right);
  d().drawString(s.c_str(), x, y);
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
  d().endWrite();
}

void injectTap(int x, int y) {
  g_tap_x = x;
  g_tap_y = y;
  g_tap_pending = true;
}

void pump() {
  if (!g_screen) return;
  Screen *s = g_screen;

  keyboard::Key k;
  while (keyboard::take(&k)) {
    s->key(k);
    if (g_screen != s) return;   // the key changed screens
  }

  if (g_tap_pending) {
    g_tap_pending = false;
    s->tap(g_tap_x, g_tap_y);
    if (g_screen != s) return;
  }

  auto t = M5.Touch.getDetail();
  if (t.isPressed()) {
    if (!g_down) {
      g_down = true;
      g_dragging = false;
      g_start_x = t.x;
      g_start_y = g_last_y = t.y;
    } else {
      if (!g_dragging && abs(t.y - g_start_y) > kDragSlop) g_dragging = true;
      if (g_dragging && t.y != g_last_y) {
        s->drag(t.y - g_last_y);
        g_last_y = t.y;
      }
    }
  } else if (g_down) {
    g_down = false;
    if (!g_dragging) s->tap(g_start_x, g_start_y);
  }
  if (g_screen == s) s->tick();
}

}  // namespace ui
}  // namespace burner
