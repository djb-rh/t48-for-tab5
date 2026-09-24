// The hex editor: the chosen image, 16 bytes a row, hex on the left and
// ASCII on the right, edited in place in PSRAM and saved back to the card.
//
// Keys: arrows, PgUp/PgDn, Home/End (row), Ctrl+Home/End (file); hex digits
// (or characters, on the ASCII side) type over bytes; Tab switches sides;
// Ctrl+G go to, Ctrl+F find, Ctrl+N find next, Ctrl+Z undo, Ctrl+S save,
// Ctrl+A save as, Esc back. Touch: tap a byte, drag to scroll, toolbar.

#include <esp_heap_caps.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cstring>

#include "app.h"
#include "runner.h"
#include "ui.h"

namespace burner {
namespace app {

using namespace ui;
using keyboard::Key;
using keyboard::Special;

namespace {

constexpr uint32_t kMaxSize = 16u << 20;
constexpr int kGridY = 136, kRowH = 24, kRows = 23;
constexpr int kCharW = 14;
constexpr int kAddrX = 36;
constexpr int kHexX = kAddrX + 10 * kCharW;           // after "00000000  "
constexpr int kAsciiX = kHexX + 16 * 3 * kCharW + 2 * kCharW;

int hexX(int col) { return kHexX + col * 3 * kCharW + (col >= 8 ? kCharW : 0); }

enum Tool { kGoto = 1, kFind, kNext, kUndo, kSave, kSaveAs, kClose };

class HexScreen : public Screen {
 public:
  bool open(const std::string &path) {
    release();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (uint32_t)n > kMaxSize) {
      fclose(f);
      runner::note("Hex editor: %s is %ld bytes; it opens files up to 16 MB", baseName(path).c_str(), n);
      return false;
    }
    size_ = (uint32_t)n;
    data_ = (uint8_t *)heap_caps_malloc(size_, MALLOC_CAP_SPIRAM);
    dirty_ = (uint8_t *)heap_caps_calloc((size_ + 7) / 8, 1, MALLOC_CAP_SPIRAM);
    const bool ok = data_ && dirty_ && fread(data_, 1, size_, f) == size_;
    fclose(f);
    if (!ok) {
      release();
      return false;
    }
    path_ = path;
    pos_ = 0;
    top_ = 0;
    nibble_ = 0;
    ascii_ = false;
    modified_ = false;
    undo_.clear();
    crc_valid_ = false;
    return true;
  }

  void draw() override {
    drawHeader();
    buttons_.clear();
    const struct { int id; const char *label, *key; } tools[] = {
        {kGoto, "Go to", "^G"}, {kFind, "Find", "^F"}, {kNext, "Next", "^N"}, {kUndo, "Undo", "^Z"},
        {kSave, "Save", "^S"},  {kSaveAs, "Save as", "^A"}, {kClose, "Close", "Esc"},
    };
    int x = 16;
    for (auto &t : tools) {
      Button b;
      b.id = t.id;
      b.x = x;
      b.y = 66;
      b.w = 170;
      b.h = 56;
      b.label = t.label;
      b.key = t.key;
      buttons_.push_back(b);
      drawButton(b);
      x += 180;
    }
    drawGrid();
    drawStatus();
  }

  void tick() override {
    // The CRC of a big image takes a moment; do it when idle, not per key.
    if (!crc_valid_ && millis() - last_key_ms_ > 400) {
      crc_ = esp_rom_crc32_le(0, data_, size_);
      crc_valid_ = true;
      d().startWrite();
      drawStatus();
      d().endWrite();
    }
  }

  void key(const Key &k) override {
    last_key_ms_ = millis();
    if (k.ctrl) {
      switch (tolower((unsigned char)k.ch)) {
        case 'g': tool(kGoto); return;
        case 'f': tool(kFind); return;
        case 'n': tool(kNext); return;
        case 'z': tool(kUndo); return;
        case 's': tool(kSave); return;
        case 'a': tool(kSaveAs); return;
      }
      if (k.special == Special::Home) return moveTo(0);
      if (k.special == Special::End) return moveTo(size_ - 1);
      return;
    }
    switch (k.special) {
      case Special::Escape: tool(kClose); return;
      case Special::Tab:
        ascii_ = !ascii_;
        nibble_ = 0;
        return refreshCursor(pos_);
      case Special::Left: return moveTo((int64_t)pos_ - 1);
      case Special::Right: return moveTo((int64_t)pos_ + 1);
      case Special::Up: return moveTo((int64_t)pos_ - 16);
      case Special::Down: return moveTo((int64_t)pos_ + 16);
      case Special::PageUp: return moveTo((int64_t)pos_ - 16 * kRows);
      case Special::PageDown: return moveTo((int64_t)pos_ + 16 * kRows);
      case Special::Home: return moveTo(pos_ & ~15u);
      case Special::End: return moveTo(std::min<int64_t>((pos_ | 15u), size_ - 1));
      case Special::Backspace: return moveTo((int64_t)pos_ - 1);
      default: break;
    }
    if (!k.ch) return;
    if (ascii_) {
      if (k.ch >= ' ' && k.ch < 127) {
        set(pos_, (uint8_t)k.ch);
        moveTo((int64_t)pos_ + 1);
      }
      return;
    }
    const int v = isdigit((unsigned char)k.ch) ? k.ch - '0'
                  : (tolower((unsigned char)k.ch) >= 'a' && tolower((unsigned char)k.ch) <= 'f')
                      ? tolower((unsigned char)k.ch) - 'a' + 10
                      : -1;
    if (v < 0) return;
    const uint8_t old = data_[pos_];
    set(pos_, nibble_ == 0 ? (uint8_t)((v << 4) | (old & 0x0F)) : (uint8_t)((old & 0xF0) | v));
    if (nibble_ == 0) {
      nibble_ = 1;
      refreshCursor(pos_);
    } else {
      nibble_ = 0;
      moveTo((int64_t)pos_ + 1);
    }
  }

  void tap(int x, int y) override {
    const int id = hit(buttons_, x, y);
    if (id > 0) return tool(id);
    if (y < kGridY || y >= kGridY + kRows * kRowH) return;
    const uint32_t row = top_ + (y - kGridY) / kRowH;
    int col = -1;
    bool ascii = false;
    if (x >= kHexX && x < kAsciiX - kCharW) {
      for (int c = 15; c >= 0; c--)
        if (x >= hexX(c)) {
          col = c;
          break;
        }
    } else if (x >= kAsciiX && x < kAsciiX + 16 * kCharW) {
      col = (x - kAsciiX) / kCharW;
      ascii = true;
    }
    if (col < 0) return;
    const uint32_t p = row * 16 + col;
    if (p >= size_) return;
    ascii_ = ascii;
    nibble_ = 0;
    moveTo(p);
  }

  void drag(int dy) override {
    drag_ += dy;
    int rows = 0;
    while (drag_ >= kRowH) { drag_ -= kRowH; rows--; }
    while (drag_ <= -kRowH) { drag_ += kRowH; rows++; }
    if (!rows) return;
    const int64_t maxTop = std::max<int64_t>(0, (int64_t)(size_ + 15) / 16 - kRows);
    top_ = (uint32_t)std::max<int64_t>(0, std::min<int64_t>((int64_t)top_ + rows, maxTop));
    // Keep the cursor on screen.
    const uint32_t first = top_ * 16, last = std::min<uint32_t>(size_ - 1, (top_ + kRows) * 16 - 1);
    pos_ = std::max(first, std::min(pos_, last));
    d().startWrite();
    drawGrid();
    drawStatus();
    d().endWrite();
  }

 private:
  std::string path_;
  uint8_t *data_ = nullptr;
  uint8_t *dirty_ = nullptr;
  uint32_t size_ = 0, pos_ = 0, top_ = 0;
  int nibble_ = 0;
  bool ascii_ = false, modified_ = false;
  std::vector<std::pair<uint32_t, uint8_t>> undo_;
  std::vector<uint8_t> pattern_;
  std::vector<Button> buttons_;
  uint32_t crc_ = 0;
  bool crc_valid_ = false;
  uint32_t last_key_ms_ = 0;
  int drag_ = 0;

  void release() {
    free(data_);
    free(dirty_);
    data_ = dirty_ = nullptr;
    size_ = 0;
  }

  bool isDirty(uint32_t p) const { return dirty_[p >> 3] & (1 << (p & 7)); }

  void set(uint32_t p, uint8_t v) {
    if (data_[p] == v) return;
    undo_.push_back({p, data_[p]});
    data_[p] = v;
    dirty_[p >> 3] |= 1 << (p & 7);
    crc_valid_ = false;
    if (!modified_) {
      modified_ = true;
      d().startWrite();
      drawHeader();
      d().endWrite();
    }
  }

  void drawHeader() {
    d().fillRect(0, 0, W, 56, kPanel);
    d().drawFastHLine(0, 56, W, kBorder);
    text(20, 14, fit("Hex: " + baseName(path_) + (modified_ ? "  (modified)" : ""), Font::Body, 800), Font::Body,
         modified_ ? kEdit : kText, kPanel);
    text(W - 20, 18, bytesText(size_), Font::Small, kDim, kPanel, 2);
  }

  void drawRow(uint32_t row) {
    const int y = kGridY + (int)(row - top_) * kRowH;
    d().fillRect(0, y, W, kRowH, kBg);
    const uint32_t base = row * 16;
    if (base >= size_) return;
    setFont(Font::Mono);
    d().setTextDatum(top_left);
    char b[12];
    snprintf(b, sizeof(b), "%08lX", (unsigned long)base);
    d().setTextColor(kDim, kBg);
    d().drawString(b, kAddrX, y + 2);
    for (int c = 0; c < 16 && base + c < size_; c++) {
      const uint32_t p = base + c;
      const uint8_t v = data_[p];
      const bool cur = p == pos_;
      const uint32_t fg = isDirty(p) ? kEdit : kText;
      // Hex side.
      uint32_t bg = cur ? (ascii_ ? kAccentDim : kAccent) : kBg;
      if (bg != kBg) d().fillRect(hexX(c) - 2, y, 2 * kCharW + 4, kRowH, bg);
      snprintf(b, sizeof(b), "%02X", v);
      d().setTextColor(fg, bg);
      d().drawString(b, hexX(c), y + 2);
      if (cur && !ascii_ && nibble_ == 1) d().drawFastHLine(hexX(c) + kCharW, y + kRowH - 3, kCharW, kText);
      // ASCII side.
      bg = cur ? (ascii_ ? kAccent : kAccentDim) : kBg;
      if (bg != kBg) d().fillRect(kAsciiX + c * kCharW, y, kCharW, kRowH, bg);
      b[0] = (v >= 32 && v < 127) ? (char)v : '.';
      b[1] = 0;
      d().setTextColor(v >= 32 && v < 127 ? fg : kFaint, bg);
      d().drawString(b, kAsciiX + c * kCharW, y + 2);
    }
  }

  void drawGrid() {
    d().fillRect(0, kGridY - 8, W, kRows * kRowH + 8, kBg);
    for (int r = 0; r < kRows; r++) drawRow(top_ + r);
  }

  void drawStatus() {
    const int y = 694;
    d().fillRect(0, y, W, H - y, kPanel);
    char b[160];
    const uint8_t v = data_[pos_];
    char bits[9];
    for (int i = 0; i < 8; i++) bits[i] = (v & (0x80 >> i)) ? '1' : '0';
    bits[8] = 0;
    snprintf(b, sizeof(b), "offset %08lX   value %02X  %3u  %s   %s", (unsigned long)pos_, v, v, bits,
             ascii_ ? "ASCII side" : "hex side");
    text(20, y + 2, b, Font::Small, kText, kPanel);
    if (crc_valid_) snprintf(b, sizeof(b), "CRC32 %08lX", (unsigned long)crc_);
    else snprintf(b, sizeof(b), "CRC32 ...");
    text(W - 20, y + 2, b, Font::Small, kDim, kPanel, 2);
  }

  void refreshCursor(uint32_t old) {
    d().startWrite();
    if (old / 16 != pos_ / 16 && old / 16 >= top_ && old / 16 < top_ + kRows) drawRow(old / 16);
    drawRow(pos_ / 16);
    drawStatus();
    d().endWrite();
  }

  void moveTo(int64_t p) {
    if (!size_) return;
    p = std::max<int64_t>(0, std::min<int64_t>(p, size_ - 1));
    const uint32_t old = pos_;
    pos_ = (uint32_t)p;
    if (pos_ != old) nibble_ = 0;
    const uint32_t row = pos_ / 16;
    if (row < top_ || row >= top_ + kRows) {
      top_ = row < top_ ? row : row - kRows + 1;
      d().startWrite();
      drawGrid();
      drawStatus();
      d().endWrite();
    } else {
      refreshCursor(old);
    }
  }

  // Back here from a prompt or a confirmation.
  void resume() { show(this); }

  bool find(bool from_next) {
    if (pattern_.empty() || pattern_.size() > size_) return false;
    const uint32_t n = size_;
    const uint32_t start = from_next ? pos_ + 1 : pos_;
    for (uint32_t k = 0; k < n; k++) {
      const uint32_t p = (start + k) % n;
      if (p + pattern_.size() > n) continue;
      if (!memcmp(data_ + p, pattern_.data(), pattern_.size())) {
        pos_ = p;
        top_ = pos_ / 16 >= (uint32_t)kRows / 2 ? pos_ / 16 - kRows / 2 : 0;
        return true;
      }
    }
    return false;
  }

  bool save(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data_, 1, size_, f) == size_;
    if (fclose(f) != 0 || !ok) return false;
    memset(dirty_, 0, (size_ + 7) / 8);
    modified_ = false;
    undo_.clear();
    return true;
  }

  void tool(int id) {
    switch (id) {
      case kGoto:
        prompt("Go to address (hex)", "", [this](bool ok, const std::string &t) {
          resume();
          if (ok && !t.empty()) moveTo((int64_t)strtoul(t.c_str(), nullptr, 16));
        });
        break;
      case kFind:
        prompt("Find: hex bytes (C3 00 10) or \"text\"", "", [this](bool ok, const std::string &t) {
          if (ok && !t.empty()) {
            pattern_.clear();
            if (t[0] == '"') {
              for (size_t i = 1; i < t.size() && t[i] != '"'; i++) pattern_.push_back((uint8_t)t[i]);
            } else {
              std::string hex;
              for (char c : t)
                if (isxdigit((unsigned char)c)) hex += c;
              for (size_t i = 0; i + 1 < hex.size(); i += 2)
                pattern_.push_back((uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
            }
            if (!find(false)) runner::note("Hex editor: not found");
          }
          resume();
        });
        break;
      case kNext:
        if (find(true)) {
          d().startWrite();
          drawGrid();
          drawStatus();
          d().endWrite();
        }
        break;
      case kUndo:
        if (!undo_.empty()) {
          const auto u = undo_.back();
          undo_.pop_back();
          data_[u.first] = u.second;
          crc_valid_ = false;
          // Only the undo history knows whether a byte is still changed.
          bool still = false;
          for (auto &e : undo_) still |= e.first == u.first;
          if (!still) dirty_[u.first >> 3] &= ~(1 << (u.first & 7));
          modified_ = !undo_.empty();
          nibble_ = 0;
          pos_ = u.first;
          if (pos_ / 16 < top_ || pos_ / 16 >= top_ + kRows) top_ = pos_ / 16;
          redraw();
        }
        break;
      case kSave:
        if (save(path_)) {
          runner::note("Saved %s", baseName(path_).c_str());
          if (path_ == image().path) refreshImage();
        } else {
          runner::note("Could not save %s", baseName(path_).c_str());
        }
        redraw();
        break;
      case kSaveAs: {
        std::string name = baseName(path_);
        const size_t dot = name.rfind('.');
        name = (dot == std::string::npos ? name : name.substr(0, dot)) + "_edit" +
               (dot == std::string::npos ? "" : name.substr(dot));
        prompt("Save as (in the same folder)", name, [this](bool ok, const std::string &t) {
          if (ok && !t.empty()) {
            const std::string dir = path_.substr(0, path_.rfind('/'));
            const std::string p = dir + "/" + t;
            if (save(p)) {
              path_ = p;
              setImage(p);   // the saved copy becomes the image to burn
              runner::note("Saved %s", t.c_str());
            } else {
              runner::note("Could not save %s", t.c_str());
            }
          }
          resume();
        });
        break;
      }
      case kClose:
        if (modified_) {
          confirm("Discard changes?", baseName(path_) + " has unsaved changes.", "Discard", [this](bool y) {
            if (y) {
              release();
              goMain();
            } else {
              resume();
            }
          });
        } else {
          release();
          goMain();
        }
        break;
    }
  }
};

HexScreen g_hex;

}  // namespace

void goHex() {
  if (!image().ok) return;
  if (!g_hex.open(image().path)) {
    runner::note("Hex editor: could not open %s", baseName(image().path).c_str());
    goMain();
    return;
  }
  show(&g_hex);
}

}  // namespace app
}  // namespace burner
