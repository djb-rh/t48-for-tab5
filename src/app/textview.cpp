// A read-only viewer for README files: Markdown gets headings, lists, code
// blocks and plain inline text (markers stripped, links as "text (url)");
// anything else is shown in a small monospaced font so column tables line up.
//
// Keys: arrows, PgUp/PgDn, Home/End scroll; Esc goes back. Touch: drag.

#include <dirent.h>

#include <algorithm>
#include <cstring>

#include "app.h"
#include "ui.h"

namespace burner {
namespace app {

using namespace ui;
using keyboard::Key;
using keyboard::Special;

namespace {

struct Line {
  std::string text;
  Font font;
  uint32_t color;
  int indent;
  int height;
};

constexpr int kTop = 70, kBottom = H - 8, kLeft = 36, kWidth = W - 72;

// **bold**, __bold__, `code` -> plain; [text](url) -> text (url).
std::string inlinePlain(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] == s[i]) {
      i++;
      continue;
    }
    if (s[i] == '`') continue;
    if (s[i] == '[') {
      const size_t close = s.find("](", i);
      const size_t end = close == std::string::npos ? std::string::npos : s.find(')', close);
      if (end != std::string::npos) {
        o += s.substr(i + 1, close - i - 1) + " (" + s.substr(close + 2, end - close - 2) + ")";
        i = end;
        continue;
      }
    }
    o += s[i];
  }
  return o;
}

class TextScreen : public Screen {
 public:
  std::function<void()> back;

  bool open(const std::string &path) {
    path_ = path;
    lines_.clear();
    top_ = 0;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && all.size() < 512 * 1024) all.append(buf, n);
    fclose(f);
    const std::string name = baseName(path);
    const bool md = name.size() > 3 && strcasecmp(name.c_str() + name.size() - 3, ".md") == 0;
    layout(all, md);
    return true;
  }

  void draw() override {
    d().fillRect(0, 0, W, 56, kPanel);
    d().drawFastHLine(0, 56, W, kBorder);
    text(20, 14, fit(baseName(path_), Font::Body, 800), Font::Body, kText, kPanel);
    text(W - 20, 18, "arrows / PgUp PgDn scroll  |  Esc goes back", Font::Small, kDim, kPanel, 2);
    drawBody();
  }

  void key(const Key &k) override {
    switch (k.special) {
      case Special::Escape:
      case Special::Backspace: return back();
      case Special::Up: return scrollTo(top_ - 1);
      case Special::Down: return scrollTo(top_ + 1);
      case Special::PageUp: return scrollTo(top_ - pageLines());
      case Special::PageDown: return scrollTo(top_ + pageLines());
      case Special::Home: return scrollTo(0);
      case Special::End: return scrollTo((int)lines_.size());
      default: break;
    }
  }

  void tap(int x, int y) override {
    if (y < 56) back();
  }

  void drag(int dy) override {
    drag_ += dy;
    int by = 0;
    while (drag_ >= 28) { drag_ -= 28; by--; }
    while (drag_ <= -28) { drag_ += 28; by++; }
    if (by) scrollTo(top_ + by);
  }

 private:
  std::string path_;
  std::vector<Line> lines_;
  int top_ = 0, drag_ = 0;

  // Greedy word wrap to the width left after the indent.
  void add(const std::string &s, Font f, uint32_t color, int indent, int gap = 0) {
    const int h = fontHeight(f) + 4 + gap;
    const int maxw = kWidth - indent;
    if (s.empty()) {
      lines_.push_back({"", f, color, indent, h / 2});
      return;
    }
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
      size_t j = s.find(' ', i);
      if (j == std::string::npos) j = s.size();
      const std::string word = s.substr(i, j - i);
      const std::string trial = cur.empty() ? word : cur + " " + word;
      if (!cur.empty() && textWidth(trial.c_str(), f) > maxw) {
        lines_.push_back({cur, f, color, indent, h});
        cur = word;
      } else {
        cur = trial;
      }
      // A single word wider than the line: cut it.
      while (textWidth(cur.c_str(), f) > maxw && cur.size() > 1) {
        size_t k = cur.size();
        while (k > 1 && textWidth(cur.substr(0, k).c_str(), f) > maxw) k--;
        lines_.push_back({cur.substr(0, k), f, color, indent, h});
        cur = cur.substr(k);
      }
      i = j + 1;
    }
    if (!cur.empty()) lines_.push_back({cur, f, color, indent, h});
  }

  void layout(const std::string &all, bool md) {
    size_t p = 0;
    bool code = false;
    while (p <= all.size()) {
      size_t e = all.find('\n', p);
      if (e == std::string::npos) e = all.size();
      std::string l = all.substr(p, e - p);
      p = e + 1;
      if (!l.empty() && l.back() == '\r') l.pop_back();
      for (char &c : l)
        if (c == '\t') c = ' ';
      if (!md) {
        add(l, Font::MonoSmall, kText, 0);
        continue;
      }
      if (l.compare(0, 3, "```") == 0) {
        code = !code;
        continue;
      }
      if (code) {
        add(l, Font::MonoSmall, kEdit, 20);
        continue;
      }
      size_t lead = 0;
      while (lead < l.size() && l[lead] == ' ') lead++;
      const std::string t = l.substr(lead);
      if (t.compare(0, 2, "# ") == 0) add(inlinePlain(t.substr(2)), Font::Big, kText, 0, 10);
      else if (t.compare(0, 3, "## ") == 0) add(inlinePlain(t.substr(3)), Font::Body, kAccent, 0, 8);
      else if (t.compare(0, 4, "### ") == 0 || t.compare(0, 5, "#### ") == 0)
        add(inlinePlain(t.substr(t.find(' ') + 1)), Font::Body, kText, 0, 4);
      else if (t.compare(0, 2, "- ") == 0 || t.compare(0, 2, "* ") == 0 || t.compare(0, 2, "+ ") == 0)
        add("-  " + inlinePlain(t.substr(2)), Font::Small, kText, 20 + (int)lead * 8);
      else if (!t.empty() && t[0] == '|') add(t, Font::MonoSmall, kText, 0);   // tables: as written
      else if (t.compare(0, 3, "---") == 0 || t.compare(0, 3, "***") == 0) add("", Font::Small, kDim, 0);
      else add(inlinePlain(l), Font::Small, kText, (int)lead * 8);
    }
  }

  int pageLines() { return std::max(1, (kBottom - kTop) / 28); }

  void scrollTo(int t) {
    t = std::max(0, std::min(t, std::max(0, (int)lines_.size() - pageLines())));
    if (t == top_) return;
    top_ = t;
    d().startWrite();
    drawBody();
    d().endWrite();
  }

  void drawBody() {
    d().fillRect(0, 57, W, H - 57, kBg);
    int y = kTop;
    for (size_t i = top_; i < lines_.size(); i++) {
      const Line &l = lines_[i];
      if (y + l.height > kBottom) break;
      if (!l.text.empty()) text(kLeft + l.indent, y, l.text, l.font, l.color, kBg);
      y += l.height;
    }
    // Where in the document we are.
    if ((int)lines_.size() > pageLines()) {
      const int track = kBottom - kTop;
      const int bar = std::max(30, track * pageLines() / (int)lines_.size());
      const int pos = (track - bar) * top_ / std::max(1, (int)lines_.size() - pageLines());
      d().fillRoundRect(W - 12, kTop + pos, 6, bar, 3, kFaint);
    }
  }
};

TextScreen g_text;

}  // namespace

void goText(const std::string &path, std::function<void()> back) {
  if (!g_text.open(path)) return;
  g_text.back = back;
  show(&g_text);
}

std::string findReadme(const std::string &dir) {
  std::string best;
  int rank = 99;
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d)) {
      const char *n = e->d_name;
      int r = 99;
      if (!strcasecmp(n, "README.md")) r = 0;
      else if (!strcasecmp(n, "README.txt")) r = 1;
      else if (!strcasecmp(n, "README")) r = 2;
      if (r < rank) {
        rank = r;
        best = dir + "/" + n;
      }
    }
    closedir(d);
  }
  return best;
}

}  // namespace app
}  // namespace burner
