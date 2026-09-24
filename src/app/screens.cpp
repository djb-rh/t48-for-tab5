// The main screen, the chip and file pickers, and the two small dialogs.
// The hex editor is in hexedit.cpp, Wi-Fi in wifi_screen.cpp.

#include <dirent.h>
#include <esp_rom_crc.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#include "app.h"
#include "parts.h"
#include "runner.h"
#include "settings.h"
#include "ui.h"

namespace burner {
namespace app {

using namespace ui;
using keyboard::Key;
using keyboard::Special;

namespace {

Image g_image;
int g_part = -1;
Status g_status;

bool isLogic() { return g_part >= 0 && !strcmp(parts::row(g_part).kind, "Logic"); }

std::string partName() { return g_part >= 0 ? parts::row(g_part).name : ""; }

// The arguments every job starts with: the part and its one-part database.
std::vector<std::string> baseArgs() {
  return {"-p", partName(), "--infoic", parts::kSelInfoic, "--logicic", parts::kSelLogicic};
}

bool fileExists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

// ============================================================================
// Main screen
// ============================================================================

enum Btn {
  kChoosePart = 1, kInfo, kChooseImage, kHex,
  kBlank, kRead, kWrite, kVerify, kErase, kId, kLogic,
  kOptSize, kOptErase, kOptVerify, kOptId, kWifi,
};

class MainScreen : public Screen {
 public:
  void draw() override {
    drawHeader();
    drawChip();
    drawImage();
    drawActions();
    drawLog();
  }

  void tick() override {
    const auto st = runner::status();
    if (st.seq != seq_) {
      seq_ = st.seq;
      if (st.busy != was_busy_) {
        was_busy_ = st.busy;
        d().startWrite();
        drawChip();
        drawImage();
        drawActions();
        d().endWrite();
        if (!st.busy) finished(st);
      }
      d().startWrite();
      drawLog();
      d().endWrite();
    }
    const uint32_t now = millis();
    if (now - header_ms_ > 1000) {
      header_ms_ = now;
      const std::string h = headerKey();
      if (h != header_) {
        // The T48 coming or going changes which actions are possible.
        d().startWrite();
        drawHeader();
        drawActions();
        d().endWrite();
      }
    }
  }

  void key(const Key &k) override {
    if (k.special != Special::None || k.ctrl) return;
    switch (toupper((unsigned char)k.ch)) {
      case 'P': press(kChoosePart); break;
      case 'D': press(kInfo); break;
      case 'F': press(kChooseImage); break;
      case 'H': press(kHex); break;
      case 'B': press(isLogic() ? -1 : kBlank); break;
      case 'R': press(kRead); break;
      case 'W': press(kWrite); break;
      case 'V': press(kVerify); break;
      case 'E': press(kErase); break;
      case 'I': press(kId); break;
      case 'T': press(kLogic); break;
      case 'N': press(kWifi); break;
      case '1': press(kOptSize); break;
      case '2': press(kOptErase); break;
      case '3': press(kOptVerify); break;
      case '4': press(kOptId); break;
    }
  }

  void tap(int x, int y) override {
    if (y < 56 && x > 900) {
      press(kWifi);
      return;
    }
    press(hit(buttons_, x, y));
  }

  // A job finished whose result the screen acts on (a read sets the image).
  std::string read_target_;

 private:
  uint32_t seq_ = 0;
  bool was_busy_ = false;
  uint32_t header_ms_ = 0;
  std::string header_;
  std::vector<Button> buttons_;

  bool busy() { return runner::busy(); }

  std::string headerKey() {
    char b[160];
    const Status &s = g_status;
    snprintf(b, sizeof(b), "%d|%d|%llu|%s|%d|%d", s.t48, s.sd, (unsigned long long)(s.sd_free >> 20),
             s.wifi.c_str(), s.battery, s.charging);
    return b;
  }

  void drawHeader() {
    header_ = headerKey();
    const Status &s = g_status;
    d().fillRect(0, 0, W, 56, kPanel);
    d().drawFastHLine(0, 56, W, kBorder);
    text(20, 14, "T48 Burner", Font::Body, kText, kPanel);
    int x = 560;
    auto item = [&](const char *label, const std::string &value, uint32_t color) {
      text(x, 18, label, Font::Small, kDim, kPanel);
      x += textWidth(label, Font::Small) + 8;
      text(x, 18, value, Font::Small, color, kPanel);
      x += textWidth(value.c_str(), Font::Small) + 30;
    };
    item("T48", s.t48 ? "connected" : "not found", s.t48 ? kGood : kBad);
    item("SD", s.sd ? bytesText(s.sd_free) + " free" : "missing", s.sd ? kText : kBad);
    item("Wi-Fi", s.wifi, s.wifi == "off" ? kDim : kText);
    char bat[24];
    if (s.battery >= 0) snprintf(bat, sizeof(bat), "%d%%%s", s.battery, s.charging ? " +" : "");
    else snprintf(bat, sizeof(bat), "--");
    text(W - 20, 18, bat, Font::Small, kText, kPanel, 2);
  }

  Button mk(int id, int x, int y, int w, int h, const char *label, const char *key) {
    Button b;
    b.id = id;
    b.x = x;
    b.y = y;
    b.w = w;
    b.h = h;
    b.label = label;
    b.key = key;
    return b;
  }

  void addButton(const Button &b) {
    buttons_.erase(std::remove_if(buttons_.begin(), buttons_.end(), [&](const Button &o) { return o.id == b.id; }),
                   buttons_.end());
    buttons_.push_back(b);
    drawButton(b);
  }

  void drawChip() {
    panel(16, 72, 608, 196, kPanel);
    text(36, 86, "CHIP", Font::Small, kDim, kPanel);
    if (g_part >= 0) {
      const auto &r = parts::row(g_part);
      text(36, 112, fit(r.name, Font::Big, 568), Font::Big, kText, kPanel);
      text(36, 160, fit(std::string(r.maker) + "  |  " + r.kind + "  |  " + parts::sizeText(r), Font::Body, 568),
           Font::Body, kDim, kPanel);
    } else {
      text(36, 112, parts::loaded() ? "No chip chosen" : "No part library", Font::Big, kDim, kPanel);
    }
    Button a = mk(kChoosePart, 36, 200, 280, 56, "Choose chip", "P");
    a.enabled = !busy() && parts::loaded();
    addButton(a);
    Button b = mk(kInfo, 330, 200, 274, 56, "Chip info", "D");
    b.enabled = !busy() && g_part >= 0;
    addButton(b);
  }

  void drawImage() {
    panel(16, 284, 608, 196, kPanel);
    text(36, 298, "IMAGE", Font::Small, kDim, kPanel);
    if (!g_image.path.empty()) {
      text(36, 324, fit(baseName(g_image.path), Font::Big, 568), Font::Big, g_image.ok ? kText : kBad, kPanel);
      std::string line;
      uint32_t color = kDim;
      if (!g_image.ok) {
        line = "missing from the card";
        color = kBad;
      } else {
        char b[64];
        snprintf(b, sizeof(b), "%s  |  CRC32 %08lX", bytesText(g_image.size).c_str(), (unsigned long)g_image.crc);
        line = b;
        if (g_part >= 0 && !isLogic() && parts::row(g_part).size != g_image.size) {
          line += "  |  size differs";
          color = kWarn;
        }
      }
      text(36, 372, fit(line, Font::Body, 568), Font::Body, color, kPanel);
    } else {
      text(36, 324, "No image chosen", Font::Big, kDim, kPanel);
    }
    Button a = mk(kChooseImage, 36, 412, 280, 56, "Choose image", "F");
    a.enabled = !busy();
    addButton(a);
    Button b = mk(kHex, 330, 412, 274, 56, "Hex edit", "H");
    b.enabled = !busy() && g_image.ok;
    addButton(b);
  }

  void drawActions() {
    const bool can = !busy() && g_part >= 0 && g_status.t48;
    const bool logic = isLogic();
    const bool img = g_image.ok;
    const int cw = 196, ch = 92;
    const int xs[3] = {640, 854, 1068};
    struct A { int id; const char *label, *key; bool en; bool danger; };
    const A acts[6] = {
        logic ? A{kLogic, "Logic test", "T", can, false} : A{kBlank, "Blank", "B", can, false},
        {kRead, "Read", "R", can && !logic, false},
        {kWrite, "Write", "W", can && !logic && img, true},
        {kVerify, "Verify", "V", can && !logic && img, false},
        {kErase, "Erase", "E", can && !logic, true},
        {kId, "Chip ID", "I", can && !logic, false},
    };
    // A logic part hides Blank; drop whichever of the pair is not showing.
    buttons_.erase(std::remove_if(buttons_.begin(), buttons_.end(),
                                  [](const Button &o) { return o.id == kBlank || o.id == kLogic; }),
                   buttons_.end());
    for (int i = 0; i < 6; i++) {
      Button b = mk(acts[i].id, xs[i % 3], 72 + (i / 3) * (ch + 12), cw, ch, acts[i].label, acts[i].key);
      b.enabled = acts[i].en;
      b.danger = acts[i].danger;
      addButton(b);
    }

    d().fillRect(640, 284, 624, 196, kBg);
    text(640, 290, "WRITE OPTIONS", Font::Small, kDim, kBg);
    auto &o = settings::get().opt;
    struct T { int id; const char *label, *key; bool on; };
    const T ts[4] = {
        {kOptSize, "Size mismatch OK", "1", o.ignore_size},
        {kOptErase, "Skip erase", "2", o.skip_erase},
        {kOptVerify, "Skip verify", "3", o.skip_verify},
        {kOptId, "Ignore ID mismatch", "4", o.ignore_id},
    };
    for (int i = 0; i < 4; i++) {
      Button b = mk(ts[i].id, 640 + (i % 2) * 318, 318 + (i / 2) * 78, 306, 66, ts[i].label, ts[i].key);
      b.toggle = true;
      b.on = ts[i].on;
      b.enabled = !busy();
      addButton(b);
    }
  }

  void drawLog() {
    const auto st = runner::status();
    panel(16, 496, 1248, 208, kPanel);
    std::string head;
    uint32_t color = kText;
    char b[120];
    if (st.busy) {
      snprintf(b, sizeof(b), "%s  |  %s", st.title.c_str(), st.phase.c_str());
      head = b;
    } else if (st.finished) {
      if (st.rc == 0) {
        snprintf(b, sizeof(b), "%s finished OK in %.1f s", st.title.c_str(), st.elapsed_ms / 1000.0f);
        color = kGood;
      } else {
        snprintf(b, sizeof(b), "%s FAILED", st.title.c_str());
        color = kBad;
      }
      head = b;
    } else {
      head = "Ready";
      color = kDim;
    }
    text(36, 508, fit(head, Font::Body, 1000), Font::Body, color, kPanel);
    if (st.busy && st.percent >= 0) {
      snprintf(b, sizeof(b), "%d%%", st.percent);
      text(1244, 508, b, Font::Body, kText, kPanel, 2);
    }
    const int pct = st.busy ? (st.percent >= 0 ? st.percent : 0) : (st.finished ? 100 : 0);
    progressBar(36, 544, 1208, 14, pct, st.busy ? kAccent : (st.finished ? (st.rc ? kBad : kGood) : kPanel2));
    const auto lines = runner::lines(5);
    int y = 572;
    for (const auto &l : lines) {
      uint32_t c = kDim;
      if (l.find("OK") != std::string::npos) c = kGood;
      if (l.find("fail") != std::string::npos || l.find("rror") != std::string::npos ||
          l.find("not found") != std::string::npos || l.find("Invalid") != std::string::npos)
        c = kBad;
      if (l.compare(0, 2, "> ") == 0) c = kFaint;
      text(36, y, fit(l, Font::Small, 1208), Font::Small, c, kPanel);
      y += 25;
    }
  }

  void finished(const runner::Status &st) {
    if (!read_target_.empty()) {
      if (st.rc == 0) setImage(read_target_);
      read_target_.clear();
      d().startWrite();
      drawImage();
      d().endWrite();
    }
  }

  std::vector<std::string> writeFlags() {
    auto &o = settings::get().opt;
    std::vector<std::string> f;
    if (o.ignore_size) f.push_back("-s");
    if (o.skip_erase) f.push_back("-e");
    if (o.skip_verify) f.push_back("-v");
    if (o.ignore_id) f.push_back("-y");
    return f;
  }

  void run(std::vector<std::string> extra, const char *title) {
    auto args = baseArgs();
    args.insert(args.end(), extra.begin(), extra.end());
    runner::start(args, title);
  }

  // A free name for a read: "<chip>.bin", then "<chip>_2.bin" and so on.
  std::string readName() {
    std::string base;
    for (char c : partName()) {
      if (c == '@') break;
      base += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    }
    if (base.empty()) base = "read";
    std::string name = base + ".bin";
    for (int n = 2; fileExists(std::string(kImages) + "/" + name); n++) name = base + "_" + std::to_string(n) + ".bin";
    return name;
  }

  void toggle(bool *v) {
    *v = !*v;
    settings::save();
    d().startWrite();
    drawActions();
    d().endWrite();
  }

  void press(int id) {
    if (id < 0) return;
    // Buttons can be pressed by key even when not drawn enabled; check here.
    for (auto &b : buttons_)
      if (b.id == id && !b.enabled) return;
    auto &o = settings::get().opt;
    switch (id) {
      case kChoosePart: goParts(); break;
      case kChooseImage: goFiles(); break;
      case kHex: goHex(); break;
      case kWifi: goWifi(); break;
      case kInfo: run({"-d", partName()}, "Chip info"); break;
      case kBlank: run({"-b"}, "Blank check"); break;
      case kId: run({"-D"}, "Chip ID"); break;
      case kLogic: run({"-T"}, "Logic test"); break;
      case kVerify: {
        std::vector<std::string> a = {"-m", g_image.path};
        if (o.ignore_size) a.push_back("-s");
        if (o.ignore_id) a.push_back("-y");
        run(a, "Verify");
        break;
      }
      case kRead:
        prompt("Read the chip into a file in burner/images", readName(), [this](bool ok, const std::string &t) {
          goMain();
          if (!ok || t.empty()) return;
          const std::string path = std::string(kImages) + "/" + t;
          auto go = [this, path]() {
            goMain();
            read_target_ = path;
            std::vector<std::string> a = {"-r", path};
            if (settings::get().opt.ignore_id) a.push_back("-y");
            run(a, "Read");
          };
          if (fileExists(path))
            confirm("Replace file?", baseName(path) + " already exists.", "Replace", [go](bool y) {
              if (y) go();
              else goMain();
            });
          else
            go();
        });
        break;
      case kWrite: {
        std::string body = "Write " + baseName(g_image.path) + " (" + bytesText(g_image.size) + ")\nto " +
                           partName() + " (" + parts::sizeText(parts::row(g_part)) + ")?";
        if (parts::row(g_part).size != g_image.size)
          body += o.ignore_size ? "\n\nThe sizes differ; 'Size mismatch OK' is on."
                                : "\n\nThe sizes differ: minipro will refuse unless\n'Size mismatch OK' is on.";
        confirm("Write chip", body, "Write", [this](bool y) {
          goMain();
          if (!y) return;
          std::vector<std::string> a = {"-w", g_image.path};
          auto f = writeFlags();
          a.insert(a.end(), f.begin(), f.end());
          run(a, "Write");
        });
        break;
      }
      case kErase:
        confirm("Erase chip", "Erase everything on " + partName() + "?", "Erase", [this](bool y) {
          goMain();
          if (y) run({"-E"}, "Erase");
        });
        break;
      case kOptSize: toggle(&o.ignore_size); break;
      case kOptErase: toggle(&o.skip_erase); break;
      case kOptVerify: toggle(&o.skip_verify); break;
      case kOptId: toggle(&o.ignore_id); break;
    }
  }
};

MainScreen g_main;

// ============================================================================
// Chip picker
// ============================================================================

class PartsScreen : public Screen {
 public:
  void enter() {
    query_.clear();
    parts::search(query_, &results_);
    sel_ = 0;
    top_ = 0;
    // Start on the chosen part, if there is one.
    if (g_part >= 0) {
      for (size_t i = 0; i < results_.size(); i++)
        if (results_[i] == g_part) {
          sel_ = (int)i;
          top_ = std::max(0, sel_ - kRows / 2);
        }
    }
  }

  void draw() override {
    d().fillRect(0, 0, W, 56, kPanel);
    d().drawFastHLine(0, 56, W, kBorder);
    text(20, 14, "Choose chip", Font::Body, kText, kPanel);
    text(W - 20, 18, "type to search  |  arrows  |  Enter picks  |  Esc goes back", Font::Small, kDim, kPanel, 2);
    drawQuery();
    drawList();
  }

  void key(const Key &k) override {
    const int n = (int)results_.size();
    switch (k.special) {
      case Special::Escape: goMain(); return;
      case Special::Enter:
        if (n) choose(results_[sel_]);
        return;
      case Special::Backspace:
        if (!query_.empty()) {
          query_.pop_back();
          research();
        }
        return;
      case Special::Up: move(-1); return;
      case Special::Down: move(1); return;
      case Special::PageUp: move(-kRows); return;
      case Special::PageDown: move(kRows); return;
      case Special::Home: move(-n); return;
      case Special::End: move(n); return;
      default: break;
    }
    if (k.ch >= ' ' && !k.ctrl && query_.size() < 40) {
      query_ += k.ch;
      research();
    }
  }

  void tap(int x, int y) override {
    if (y < 56) {
      goMain();
      return;
    }
    if (y >= kListY && y < kListY + kRows * kRowH) {
      const int i = top_ + (y - kListY) / kRowH;
      if (i < (int)results_.size()) choose(results_[i]);
    }
  }

  void drag(int dy) override {
    drag_ += dy;
    int rows = 0;
    while (drag_ >= kRowH) { drag_ -= kRowH; rows--; }
    while (drag_ <= -kRowH) { drag_ += kRowH; rows++; }
    if (!rows) return;
    const int n = (int)results_.size();
    top_ = std::max(0, std::min(top_ + rows, std::max(0, n - kRows)));
    sel_ = std::max(top_, std::min(sel_, top_ + kRows - 1));
    d().startWrite();
    drawList();
    d().endWrite();
  }

 private:
  static constexpr int kListY = 148, kRowH = 50, kRows = 11;
  std::string query_;
  std::vector<int> results_;
  int sel_ = 0, top_ = 0, drag_ = 0;

  void research() {
    parts::search(query_, &results_);
    sel_ = 0;
    top_ = 0;
    d().startWrite();
    drawQuery();
    drawList();
    d().endWrite();
  }

  void move(int by) {
    const int n = (int)results_.size();
    if (!n) return;
    sel_ = std::max(0, std::min(n - 1, sel_ + by));
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + kRows) top_ = sel_ - kRows + 1;
    d().startWrite();
    drawList();
    d().endWrite();
  }

  void choose(int row) {
    std::string why;
    if (!setPart(row, &why)) runner::note("%s", why.c_str());
    goMain();
  }

  void drawQuery() {
    panel(16, 72, 1248, 60, kPanel2, kAccent);
    if (query_.empty()) {
      char b[80];
      snprintf(b, sizeof(b), "Type part of a name (%d parts)", parts::count());
      text(36, 88, b, Font::Body, kFaint, kPanel2);
    } else {
      text(36, 88, query_ + "_", Font::Body, kText, kPanel2);
    }
    char c[40];
    snprintf(c, sizeof(c), "%d match%s", (int)results_.size(), results_.size() == 1 ? "" : "es");
    text(1244, 90, c, Font::Small, kDim, kPanel2, 2);
  }

  void drawList() {
    d().fillRect(0, kListY, W, kRows * kRowH, kBg);
    for (int r = 0; r < kRows; r++) {
      const int i = top_ + r;
      if (i >= (int)results_.size()) break;
      const auto &p = parts::row(results_[i]);
      const int y = kListY + r * kRowH;
      const bool s = i == sel_;
      const uint32_t bg = s ? kAccentDim : (r % 2 ? kBg : kPanel);
      d().fillRect(16, y, 1248, kRowH - 2, bg);
      text(36, y + 12, fit(p.name, Font::Body, 560), Font::Body, kText, bg);
      text(620, y + 15, fit(p.maker, Font::Small, 300), Font::Small, s ? kText : kDim, bg);
      text(940, y + 15, p.kind, Font::Small, s ? kText : kDim, bg);
      text(1244, y + 15, parts::sizeText(p), Font::Small, s ? kText : kDim, bg, 2);
    }
    if (results_.empty()) text(W / 2, kListY + 40, "No parts match", Font::Body, kDim, kBg, 1);
  }
};

PartsScreen g_parts;

// ============================================================================
// Image picker: burner/images and its folders
// ============================================================================

class FilesScreen : public Screen {
 public:
  void enter() {
    if (dir_.empty() || dir_.compare(0, strlen(kImages), kImages) != 0) dir_ = kImages;
    scan();
  }

  void draw() override {
    d().fillRect(0, 0, W, 56, kPanel);
    d().drawFastHLine(0, 56, W, kBorder);
    text(20, 14, "Choose image", Font::Body, kText, kPanel);
    text(W - 20, 18, "arrows  |  Enter picks  |  Del deletes  |  Esc goes back", Font::Small, kDim, kPanel, 2);
    std::string where = dir_.substr(strlen("/sdcard/"));
    text(36, 72, where, Font::Body, kDim, kBg);
    drawList();
  }

  void key(const Key &k) override {
    const int n = (int)items_.size();
    switch (k.special) {
      case Special::Escape: goMain(); return;
      case Special::Enter:
        if (n) open(sel_);
        return;
      case Special::Backspace:
        if (dir_ != kImages) {
          up();
          return;
        }
        goMain();
        return;
      case Special::Delete:
        if (n && !items_[sel_].dir) {
          const std::string p = dir_ + "/" + items_[sel_].name;
          confirm("Delete file", "Delete " + items_[sel_].name + " from the card?", "Delete", [p](bool y) {
            if (y) remove(p.c_str());
            goFiles();
          });
        }
        return;
      case Special::Up: move(-1); return;
      case Special::Down: move(1); return;
      case Special::PageUp: move(-kRows); return;
      case Special::PageDown: move(kRows); return;
      case Special::Home: move(-n); return;
      case Special::End: move(n); return;
      default: break;
    }
  }

  void tap(int x, int y) override {
    if (y < 56) {
      goMain();
      return;
    }
    if (y >= kListY && y < kListY + kRows * kRowH) {
      const int i = top_ + (y - kListY) / kRowH;
      if (i < (int)items_.size()) open(i);
    }
  }

  void drag(int dy) override {
    drag_ += dy;
    int rows = 0;
    while (drag_ >= kRowH) { drag_ -= kRowH; rows--; }
    while (drag_ <= -kRowH) { drag_ += kRowH; rows++; }
    if (!rows) return;
    const int n = (int)items_.size();
    top_ = std::max(0, std::min(top_ + rows, std::max(0, n - kRows)));
    sel_ = std::max(top_, std::min(sel_, top_ + kRows - 1));
    d().startWrite();
    drawList();
    d().endWrite();
  }

 private:
  struct Item {
    std::string name;
    bool dir;
    uint32_t size;
  };
  static constexpr int kListY = 116, kRowH = 52, kRows = 11;
  std::string dir_;
  std::vector<Item> items_;
  int sel_ = 0, top_ = 0, drag_ = 0;

  void scan() {
    items_.clear();
    mkdir("/sdcard/burner", 0777);
    mkdir(kImages, 0777);
    if (dir_ != kImages) items_.push_back({"..", true, 0});
    if (DIR *dd = opendir(dir_.c_str())) {
      while (dirent *e = readdir(dd)) {
        if (e->d_name[0] == '.') continue;
        const std::string p = dir_ + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        items_.push_back({e->d_name, S_ISDIR(st.st_mode), (uint32_t)st.st_size});
      }
      closedir(dd);
    }
    std::sort(items_.begin() + (dir_ != kImages ? 1 : 0), items_.end(), [](const Item &a, const Item &b) {
      if (a.dir != b.dir) return a.dir;
      return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    sel_ = 0;
    top_ = 0;
    // Start on the current image when it is in this folder.
    for (size_t i = 0; i < items_.size(); i++)
      if (dir_ + "/" + items_[i].name == g_image.path) {
        sel_ = (int)i;
        top_ = std::max(0, sel_ - kRows / 2);
      }
  }

  void up() {
    dir_ = dir_.substr(0, dir_.rfind('/'));
    scan();
    redraw();
  }

  void open(int i) {
    const Item &it = items_[i];
    if (it.name == "..") {
      up();
    } else if (it.dir) {
      dir_ += "/" + it.name;
      scan();
      redraw();
    } else {
      setImage(dir_ + "/" + it.name);
      goMain();
    }
  }

  void move(int by) {
    const int n = (int)items_.size();
    if (!n) return;
    sel_ = std::max(0, std::min(n - 1, sel_ + by));
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + kRows) top_ = sel_ - kRows + 1;
    d().startWrite();
    drawList();
    d().endWrite();
  }

  void drawList() {
    d().fillRect(0, kListY, W, kRows * kRowH, kBg);
    for (int r = 0; r < kRows; r++) {
      const int i = top_ + r;
      if (i >= (int)items_.size()) break;
      const Item &it = items_[i];
      const int y = kListY + r * kRowH;
      const bool s = i == sel_;
      const uint32_t bg = s ? kAccentDim : (r % 2 ? kBg : kPanel);
      d().fillRect(16, y, 1248, kRowH - 2, bg);
      text(36, y + 13, it.dir ? "DIR" : "", Font::Small, kWarn, bg);
      text(100, y + 12, fit(it.name, Font::Body, 900), Font::Body, kText, bg);
      if (!it.dir) text(1244, y + 15, bytesText(it.size), Font::Small, s ? kText : kDim, bg, 2);
    }
    if (items_.empty())
      text(W / 2, kListY + 40, "No images yet: read a chip, or upload over Wi-Fi", Font::Body, kDim, kBg, 1);
  }
};

FilesScreen g_files;

// ============================================================================
// Text prompt and confirmation
// ============================================================================

class PromptScreen : public Screen {
 public:
  std::string title, value;
  std::function<void(bool, const std::string &)> done;

  void draw() override {
    panel(140, 180, 1000, 300, kPanel);
    text(180, 210, title, Font::Body, kText, kPanel);
    drawField();
    buttons_.clear();
    Button ok;
    ok.id = 1; ok.x = 700; ok.y = 390; ok.w = 200; ok.h = 64; ok.label = "OK"; ok.key = "Enter";
    Button no;
    no.id = 2; no.x = 920; no.y = 390; no.w = 200; no.h = 64; no.label = "Cancel"; no.key = "Esc";
    buttons_ = {ok, no};
    for (auto &b : buttons_) drawButton(b);
  }

  void key(const Key &k) override {
    if (k.special == Special::Enter) finish(true);
    else if (k.special == Special::Escape) finish(false);
    else if (k.special == Special::Backspace) {
      if (!value.empty()) value.pop_back();
      redrawField();
    } else if (k.ch >= ' ' && !k.ctrl && value.size() < 60) {
      value += k.ch;
      redrawField();
    }
  }

  void tap(int x, int y) override {
    const int id = hit(buttons_, x, y);
    if (id == 1) finish(true);
    if (id == 2) finish(false);
  }

 private:
  std::vector<Button> buttons_;
  void drawField() {
    panel(180, 270, 920, 64, kPanel2, kAccent);
    text(200, 288, fit(value + "_", Font::Body, 880), Font::Body, kText, kPanel2);
  }
  void redrawField() {
    d().startWrite();
    drawField();
    d().endWrite();
  }
  void finish(bool ok) {
    auto cb = done;
    const std::string v = value;
    cb(ok, v);
  }
};

PromptScreen g_prompt;

class ConfirmScreen : public Screen {
 public:
  std::string title, body, yes;
  std::function<void(bool)> done;

  void draw() override {
    panel(140, 150, 1000, 380, kPanel);
    text(180, 180, title, Font::Big, kText, kPanel);
    int y = 250;
    size_t start = 0;
    while (start <= body.size()) {
      size_t nl = body.find('\n', start);
      if (nl == std::string::npos) nl = body.size();
      text(180, y, fit(body.substr(start, nl - start), Font::Body, 920), Font::Body, kText, kPanel);
      y += 34;
      start = nl + 1;
    }
    Button ok;
    ok.id = 1; ok.x = 700; ok.y = 440; ok.w = 200; ok.h = 64; ok.label = yes; ok.key = "Enter"; ok.danger = true;
    Button no;
    no.id = 2; no.x = 920; no.y = 440; no.w = 200; no.h = 64; no.label = "Cancel"; no.key = "Esc";
    buttons_ = {ok, no};
    for (auto &b : buttons_) drawButton(b);
  }

  void key(const Key &k) override {
    if (k.special == Special::Enter) done(true);
    else if (k.special == Special::Escape) done(false);
  }

  void tap(int x, int y) override {
    const int id = hit(buttons_, x, y);
    if (id == 1) done(true);
    if (id == 2) done(false);
  }

 private:
  std::vector<Button> buttons_;
};

ConfirmScreen g_confirm;

uint32_t crcFile(const std::string &path, uint32_t *size, bool *ok) {
  *ok = false;
  *size = 0;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return 0;
  static uint8_t buf[16384];
  uint32_t crc = 0;
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    crc = esp_rom_crc32_le(crc, buf, n);
    *size += n;
  }
  fclose(f);
  *ok = true;
  return crc;
}

}  // namespace

// ============================================================================

void begin() {
  mkdir("/sdcard/burner", 0777);
  mkdir(kImages, 0777);
  auto &s = settings::get();
  if (parts::loaded() && !s.part_name.empty()) {
    const int r = parts::find(s.part_name.c_str(), s.part_maker.c_str());
    std::string why;
    if (r >= 0 && !setPart(r, &why)) runner::note("%s", why.c_str());
  }
  if (!s.image.empty()) {
    g_image.path = s.image;
    refreshImage();
  }
}

const Image &image() { return g_image; }

void refreshImage() {
  if (g_image.path.empty()) {
    g_image = Image();
    return;
  }
  g_image.crc = crcFile(g_image.path, &g_image.size, &g_image.ok);
}

void setImage(const std::string &path) {
  g_image.path = path;
  refreshImage();
  settings::get().image = path;
  settings::save();
}

int part() { return g_part; }

bool setPart(int row, std::string *why) {
  if (!parts::select(row, why)) return false;
  g_part = row;
  auto &s = settings::get();
  s.part_name = parts::row(row).name;
  s.part_maker = parts::row(row).maker;
  settings::save();
  return true;
}

Status &status() { return g_status; }

void goMain() { show(&g_main); }
void goParts() {
  g_parts.enter();
  show(&g_parts);
}
void goFiles() {
  g_files.enter();
  show(&g_files);
}

void prompt(const std::string &title, const std::string &initial,
            std::function<void(bool, const std::string &)> done) {
  g_prompt.title = title;
  g_prompt.value = initial;
  g_prompt.done = done;
  show(&g_prompt);
}

void confirm(const std::string &title, const std::string &body, const std::string &yes,
             std::function<void(bool)> done) {
  g_confirm.title = title;
  g_confirm.body = body;
  g_confirm.yes = yes;
  g_confirm.done = done;
  show(&g_confirm);
}

std::string baseName(const std::string &path) {
  const size_t s = path.rfind('/');
  return s == std::string::npos ? path : path.substr(s + 1);
}

std::string bytesText(uint64_t n) {
  char b[40];
  if (n >= 10ull << 30) snprintf(b, sizeof(b), "%.0f GB", n / 1073741824.0);
  else if (n >= 1ull << 30) snprintf(b, sizeof(b), "%.1f GB", n / 1073741824.0);
  else if (n >= 10ull << 20) snprintf(b, sizeof(b), "%.0f MB", n / 1048576.0);
  else if (n >= 1ull << 20) snprintf(b, sizeof(b), "%.1f MB", n / 1048576.0);
  else {
    // Exact below a megabyte: EPROM sizes are what people check against.
    char digits[24];
    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)n);
    std::string s = digits;
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    snprintf(b, sizeof(b), "%s bytes", s.c_str());
  }
  return b;
}

}  // namespace app
}  // namespace burner
