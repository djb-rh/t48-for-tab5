// Wi-Fi: where the file manager is, and choosing a network.

#include "app.h"
#include "ui.h"
#include "web.h"

namespace burner {
namespace app {

using namespace ui;
using keyboard::Key;
using keyboard::Special;

namespace {

enum { kScan = 1, kOther, kPhone, kBack };

class WifiScreen : public Screen {
 public:
  void draw() override {
    d().fillRect(0, 0, W, 56, kPanel);
    d().drawFastHLine(0, 56, W, kBorder);
    text(20, 14, "Wi-Fi and files", Font::Body, kText, kPanel);
    text(W - 20, 18, "S scans  |  Enter picks  |  Esc goes back", Font::Small, kDim, kPanel, 2);
    drawStatus();
    buttons_.clear();
    const struct { int id; const char *label, *key; int x; } bs[] = {{kScan, "Scan here", "S", 16},
                                                                    {kOther, "Other network", "O", 330},
                                                                    {kPhone, "Set up from phone", "F", 644},
                                                                    {kBack, "Back", "Esc", 958}};
    for (auto &b : bs) {
      Button x;
      x.id = b.id;
      x.x = b.x;
      x.y = 236;
      x.w = 306;
      x.h = 60;
      x.label = b.label;
      x.key = b.key;
      buttons_.push_back(x);
      drawButton(x);
    }
    drawList();
  }

  void tick() override {
    const std::string s = web::statusText() + (web::portalActive() ? "+" : "");
    if (s != shown_) redraw();   // the portal panel comes and goes with it
  }

  void key(const Key &k) override {
    if (k.special == Special::Escape) return goMain();
    if (k.special == Special::Up && sel_ > 0) {
      sel_--;
      return redrawList();
    }
    if (k.special == Special::Down && sel_ + 1 < (int)nets_.size()) {
      sel_++;
      return redrawList();
    }
    if (k.special == Special::Enter && !nets_.empty()) return choose(sel_);
    if (!k.ctrl && (k.ch == 's' || k.ch == 'S')) return doScan();
    if (!k.ctrl && (k.ch == 'o' || k.ch == 'O')) return other();
    if (!k.ctrl && (k.ch == 'f' || k.ch == 'F')) return phone();
  }

  void tap(int x, int y) override {
    switch (hit(buttons_, x, y)) {
      case kScan: return doScan();
      case kOther: return other();
      case kPhone: return phone();
      case kBack: return goMain();
    }
    if (y >= kListY && y < kListY + kRows * kRowH) {
      const int i = (y - kListY) / kRowH;
      if (i < (int)nets_.size()) choose(i);
    }
  }

 private:
  static constexpr int kListY = 316, kRowH = 44, kRows = 9;
  std::vector<Button> buttons_;
  std::vector<web::Net> nets_;
  int sel_ = 0;
  std::string shown_;

  void drawStatus() {
    shown_ = web::statusText() + (web::portalActive() ? "+" : "");
    panel(16, 72, 1248, 148, kPanel);
    const auto st = web::state();
    std::string net = web::ssid().empty() ? "No network chosen" : web::ssid();
    text(36, 86, "NETWORK", Font::Small, kDim, kPanel);
    const char *what = st == web::State::Connected ? "connected" : st == web::State::Connecting ? "joining..."
                       : st == web::State::Failed ? "could not join"
                       : web::portalActive() ? "setup hotspot is on" : "off";
    text(36, 110, net + "  (" + what + ")", Font::Body, st == web::State::Failed ? kBad : kText, kPanel);
    if (st == web::State::Connected) {
      text(36, 150, "Open  http://" + web::ip() + "/  in a browser to upload and download images", Font::Body, kGood,
           kPanel);
    } else {
      text(36, 150, "The file manager appears here once the Tab5 is on a network (2.4 GHz only)", Font::Body, kDim,
           kPanel);
    }
  }

  // While the setup hotspot is up, the list area shows how to use it instead:
  // a QR code that joins the hotspot, and what to do next.
  void drawPortal() {
    d().fillRect(0, kListY, W, kRows * kRowH + 4, kBg);
    const std::string ap = web::portalSsid();
    const std::string qr = "WIFI:T:nopass;S:" + ap + ";;";
    d().fillRoundRect(16, kListY, 380, 380, 12, 0xFFFFFF);
    d().qrcode(qr.c_str(), 36, kListY + 20, 340, 3);
    text(430, kListY + 10, "Set up from a phone", Font::Big, kText, kBg);
    text(430, kListY + 76, "1.  Scan the code, or join the open Wi-Fi network", Font::Body, kText, kBg);
    text(470, kListY + 112, ap, Font::Body, kGood, kBg);
    text(430, kListY + 160, "2.  The setup page opens by itself (or go to", Font::Body, kText, kBg);
    text(470, kListY + 196, "http://192.168.4.1/setup)", Font::Body, kGood, kBg);
    text(430, kListY + 244, "3.  Pick your network and type its password", Font::Body, kText, kBg);
    text(430, kListY + 300, "The hotspot closes a minute after the Tab5 joins.", Font::Small, kDim, kBg);
  }

  void drawList() {
    if (web::portalActive() && nets_.empty()) return drawPortal();
    d().fillRect(0, kListY, W, kRows * kRowH + 4, kBg);
    for (int i = 0; i < (int)nets_.size() && i < kRows; i++) {
      const auto &n = nets_[i];
      const int y = kListY + i * kRowH;
      const uint32_t bg = i == sel_ ? kAccentDim : (i % 2 ? kBg : kPanel);
      d().fillRect(16, y, 1248, kRowH - 2, bg);
      text(36, y + 9, fit(n.ssid, Font::Body, 900), Font::Body, kText, bg);
      char b[32];
      snprintf(b, sizeof(b), "%s%d dBm", n.open ? "open  " : "", n.rssi);
      text(1244, y + 12, b, Font::Small, kDim, bg, 2);
    }
  }

  void redrawList() {
    d().startWrite();
    drawList();
    d().endWrite();
  }

  void doScan() {
    d().startWrite();
    d().fillRect(0, kListY, W, kRows * kRowH + 4, kBg);
    text(36, kListY + 10, "Scanning...", Font::Body, kDim, kBg);
    d().endWrite();
    nets_ = web::scan();
    sel_ = 0;
    redrawList();
    if (nets_.empty()) {
      d().startWrite();
      text(36, kListY + 10, "No networks found", Font::Body, kDim, kBg);
      d().endWrite();
    }
  }

  void askPassword(const std::string &ssid) {
    prompt("Password for " + ssid, "", [this, ssid](bool ok, const std::string &pass) {
      if (ok) web::join(ssid, pass);
      show(this);
    });
  }

  void choose(int i) {
    const auto n = nets_[i];
    if (n.open) {
      web::join(n.ssid, "");
      redraw();
    } else {
      askPassword(n.ssid);
    }
  }

  void phone() {
    nets_.clear();
    web::setupFromPhone();
    redraw();
  }

  void other() {
    prompt("Network name (SSID)", "", [this](bool ok, const std::string &ssid) {
      if (ok && !ssid.empty()) askPassword(ssid);
      else show(this);
    });
  }
};

WifiScreen g_wifi;

}  // namespace

void goWifi() { show(&g_wifi); }

}  // namespace app
}  // namespace burner
