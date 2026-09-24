// Drawing and input plumbing shared by every screen.
//
// Everything draws straight to the panel (a full-screen PSRAM sprite costs
// ~750 ms to push on this board), so screens redraw in full only when they
// change, and repaint just the parts that move (a progress bar, one list
// row) in between.
#pragma once

#include <M5Unified.h>

#include <cstdint>
#include <string>
#include <vector>

#include "keyboard.h"

namespace burner {
namespace ui {

constexpr int W = 1280, H = 720;

// Colours as 0xRRGGBB (LovyanGFX takes a uint32_t as RGB888).
constexpr uint32_t kBg = 0x0D1117, kPanel = 0x161B22, kPanel2 = 0x21262D, kBorder = 0x30363D;
constexpr uint32_t kText = 0xE6EDF3, kDim = 0x8B949E, kFaint = 0x484F58;
constexpr uint32_t kAccent = 0x2F81F7, kAccentDim = 0x1F4E8C;
constexpr uint32_t kGood = 0x3FB950, kBad = 0xF85149, kWarn = 0xD29922, kEdit = 0xFFA657;

enum class Font : uint8_t { Small, Body, Big, Mono };

M5GFX &d();
void setFont(Font f);
int textWidth(const char *s, Font f);
int fontHeight(Font f);
// Draws s with its top-left at (x, y). align: 0 left, 1 centre, 2 right of x.
void text(int x, int y, const std::string &s, Font f, uint32_t fg, uint32_t bg, int align = 0);
// Shortens s with "..." until it fits in max_w pixels.
std::string fit(const std::string &s, Font f, int max_w);

void panel(int x, int y, int w, int h, uint32_t fill, uint32_t border = kBorder);

struct Button {
  int id = 0;
  int x = 0, y = 0, w = 0, h = 0;
  std::string label;
  std::string key;          // shortcut shown in the corner, e.g. "W"
  bool enabled = true;
  bool on = false;          // toggles: drawn lit
  bool toggle = false;
  bool danger = false;      // red accent (write, erase)
};
void drawButton(const Button &b);
// The id of the enabled button at (x, y), or -1.
int hit(const std::vector<Button> &bs, int x, int y);

void progressBar(int x, int y, int w, int h, int percent, uint32_t color);

// ---- screens ----------------------------------------------------------------

class Screen {
 public:
  virtual ~Screen() = default;
  virtual void draw() = 0;                        // everything
  virtual void tick() {}                          // cheap, every loop
  virtual void key(const keyboard::Key &k) {}
  virtual void tap(int x, int y) {}
  // A finger dragged vertically by dy pixels since the last call (lists).
  virtual void drag(int dy) {}
};

void show(Screen *s);          // makes s current and draws it
Screen *current();
void redraw();                 // full redraw of the current screen

// Called by main.cpp every loop: reads touch and the keyboard, routes them.
void pump();

// Tap injection from the serial console.
void injectTap(int x, int y);

}  // namespace ui
}  // namespace burner
