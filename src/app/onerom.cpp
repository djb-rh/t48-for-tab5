// One ROM on the USB-A port (see onerom.h).

#include "onerom.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>
#include <sys/stat.h>

#include <dirent.h>

#include <algorithm>
#include <cstring>
#include <ctime>

#include "../core/onerom_ffi.h"
#include "../core/usb_esp.h"
#include "app.h"
#include "runner.h"
#include "settings.h"

extern "C" void ort_panic(const uint8_t *msg, size_t len) {
  Serial.printf("onerom-ffi panic: %.*s\n", (int)len, (const char *)msg);
  Serial.flush();
  abort();
}

// The TLS root store ESP-IDF builds in (the same one esp_crt_bundle uses).
extern const uint8_t x509_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace burner {
namespace onerom {
namespace {

constexpr const char *kSite = "https://images.onerom.org";
constexpr const char *kUsbPlugin = "https://images.onerom.org/plugins/system/usb";
constexpr uint32_t kFlash = 0x10000000, kLive = 0x90000000;
constexpr uint32_t kChunk = 4096;

SemaphoreHandle_t g_mux = xSemaphoreCreateMutex();
Info g_info;
uint32_t g_info_gen = 0;           // the attach g_info describes
volatile uint32_t g_seen_gen = 0;  // the attach tick() has handled (a job's probe counts)
std::string g_latest;              // newest firmware for the attached board

struct Lock {
  Lock() { xSemaphoreTake(g_mux, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(g_mux); }
};

// A buffer in PSRAM (images are a few hundred KB; internal RAM feeds Wi-Fi).
struct Buf {
  uint8_t *p = nullptr;
  size_t n = 0;
  Buf() = default;
  explicit Buf(size_t size) { resize(size); }
  Buf(const Buf &) = delete;
  Buf &operator=(const Buf &) = delete;
  ~Buf() { free(p); }
  bool resize(size_t size) {
    free(p);
    p = (uint8_t *)heap_caps_malloc(size ? size : 1, MALLOC_CAP_SPIRAM);
    n = p ? size : 0;
    return p != nullptr;
  }
};

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    const size_t b = s.find(sep, a);
    out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return out;
}

// "key=value" lines.
std::string value(const std::string &lines, const char *key) {
  const std::string k = std::string(key) + "=";
  for (const auto &l : split(lines, '\n'))
    if (l.compare(0, k.size(), k) == 0) return l.substr(k.size());
  return "";
}

bool exists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

void mkdirs(const std::string &path) {
  for (size_t i = 1; i < path.size(); i++)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
  mkdir(path.c_str(), 0777);
}

bool readFile(const std::string &path, Buf *b) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  bool ok = n >= 0 && b->resize(n) && fread(b->p, 1, n, f) == (size_t)n;
  fclose(f);
  return ok;
}

bool readText(const std::string &path, std::string *s) {
  Buf b;
  if (!readFile(path, &b)) return false;
  s->assign((const char *)b.p, b.n);
  return true;
}

bool writeFile(const std::string &path, const uint8_t *data, size_t n) {
  const std::string tmp = path + ".part";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(data, 1, n, f) == n;
  if (fclose(f) != 0 || !ok) {
    remove(tmp.c_str());
    return false;
  }
  remove(path.c_str());
  return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string now() {
  const time_t t = time(nullptr);
  if (t < 1700000000) return "";   // no clock yet (no Wi-Fi since boot)
  struct tm tm;
  localtime_r(&t, &tm);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tm);
  return b;
}

std::string hex(const uint8_t *d, size_t n) {
  static const char *k = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += k[d[i] >> 4];
    s += k[d[i] & 15];
  }
  return s;
}

// ---- device I/O -------------------------------------------------------------

int readMem(uint32_t addr, uint8_t *buf, uint32_t len) {
  while (len) {
    const uint32_t n = len < kChunk ? len : kChunk;
    const uint32_t a[2] = {addr, n};
    int rc = usbdev::picoboot(0x84, a, 8, buf, n);
    if (rc) rc = usbdev::picoboot(0x84, a, 8, buf, n);   // once more after the stall recovery
    if (rc) return rc;
    addr += n;
    buf += n;
    len -= n;
  }
  return 0;
}

extern "C" int readCb(void *, uint32_t addr, uint8_t *buf, uint32_t len) { return readMem(addr, buf, len); }

// Waits for the One ROM to come back after a reboot: a new attach, in the
// wanted state.
bool waitAttach(uint32_t old_gen, bool want_boot, uint32_t timeout_ms) {
  const uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    if (usbdev::oneRomGeneration() != old_gen && usbdev::oneRomAttached() &&
        usbdev::oneRomBootloader() == want_boot) {
      delay(200);   // let it settle before the first command
      return true;
    }
    delay(50);
  }
  return false;
}

bool reboot(bool to_boot) {
  const uint32_t gen = usbdev::oneRomGeneration();
  const uint32_t a[4] = {to_boot ? 2u : 0u, 100, 0, 0};   // REBOOT2: 2 BOOTSEL, 0 normal; 100 ms
  usbdev::picoboot(0x0a, a, 16, nullptr, 0);   // the ack can be lost to the reboot
  return waitAttach(gen, to_boot, 15000);
}

// ---- probe -----------------------------------------------------------------

Info parse(const std::string &t) {
  Info in;
  in.valid = true;
  in.recognised = value(t, "recognised") == "1";
  in.running = value(t, "running") == "1";
  in.usb = value(t, "usb") == "1";
  in.board = value(t, "board");
  in.version = value(t, "version");
  in.mcu = value(t, "mcu");
  const std::string sz = value(t, "size");
  in.size = sz.find("(L)") != std::string::npos ? "L" : "M";
  for (const auto &l : split(t, '\n')) {
    if (l.compare(0, 5, "slot=") == 0) {
      const auto f = split(l.substr(5), '\t');
      if (f.size() < 7) continue;
      Slot s;
      s.index = atoi(f[0].c_str());
      s.user = atoi(f[1].c_str());
      s.plugin = f[2] == "plugin";
      s.active = f[3] == "1";
      s.type = f[4];
      s.size = strtoul(f[5].c_str(), nullptr, 10);
      s.file = f[6];
      in.slots.push_back(s);
    } else if (l.compare(0, 6, "error=") == 0) {
      in.errors.push_back(l.substr(6));
    }
  }
  return in;
}

int probe(bool quiet) {
  if (!usbdev::oneRomAttached()) {
    if (!quiet) runner::note("No One ROM on the USB-A port");
    return 1;
  }
  const uint32_t gen = usbdev::oneRomGeneration();
  g_seen_gen = gen;   // a job looked at it: tick() need not (it would replace the job's result)
  runner::setPhase("Reading the One ROM");
  Buf out(8192);
  ort_parse_device(readCb, nullptr, (char *)out.p, out.n);
  Info in = parse((const char *)out.p);
  in.running = in.running || !usbdev::oneRomBootloader();
  {
    Lock l;
    g_info = in;
    g_info_gen = gen;
  }
  if (!in.board.empty()) {
    Notes n;
    loadNotes(usbdev::oneRomSerial(), &n);
    if (n.board != in.board) {
      n.board = in.board;
      saveNotes(usbdev::oneRomSerial(), n);
    }
  }
  if (!quiet) {
    Notes n;
    if (loadNotes(usbdev::oneRomSerial(), &n) && !n.name.empty())
      runner::note("%s%s%s", n.name.c_str(), n.notes.empty() ? "" : ": ",
                   n.notes.substr(0, n.notes.find('\n')).c_str());
    runner::note("One ROM %s, %s, firmware %s, %s", usbdev::oneRomSerial().c_str(),
                 in.board.empty() ? "board unknown" : boardLabel(in.board).c_str(),
                 in.version.empty() ? "unknown" : in.version.c_str(),
                 usbdev::oneRomBootloader() ? "stopped (bootloader)" : "running");
    for (const auto &s : in.slots) {
      if (s.plugin)
        runner::note("  plugin: %s", s.file.c_str());
      else
        runner::note("  slot %d: %s, %s%s%s", s.user, s.type.c_str(), app::bytesText(s.size).c_str(),
                     s.file.empty() ? "" : (", " + s.file).c_str(), s.active ? "  (serving)" : "");
    }
    for (const auto &e : in.errors) runner::note("  parse: %s", e.c_str());
  }
  return in.recognised || usbdev::oneRomBootloader() ? 0 : 1;
}

// ---- downloads and the cache -------------------------------------------------

bool online() { return WiFi.status() == WL_CONNECTED; }

// https://images.onerom.org/a/b.bin -> /sdcard/burner/onerom/cache/a/b.bin
std::string cachePath(const std::string &url) {
  const std::string pre = std::string(kSite) + "/";
  return std::string(kCache) + "/" + (url.compare(0, pre.size(), pre) == 0 ? url.substr(pre.size()) : url);
}

// Fetches url into path (TLS checked against ESP-IDF's root store). Small
// files only: manifests, firmware and plugins are tens of KB.
bool download(const std::string &url, const std::string &path, std::string *why) {
  if (!online()) {
    *why = "no Wi-Fi to download " + url;
    return false;
  }
  NetworkClientSecure client;
  client.setCACertBundle(x509_bundle_start, x509_bundle_end - x509_bundle_start);
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url.c_str())) {
    *why = "cannot reach " + url;
    return false;
  }
  const int code = http.GET();
  if (code != 200) {
    *why = "HTTP " + std::to_string(code) + " for " + url;
    http.end();
    return false;
  }
  const int len = http.getSize();
  Buf b(len > 0 ? len : 256 * 1024);
  size_t got = 0;
  NetworkClient *s = http.getStreamPtr();
  const uint32_t t0 = millis();
  while (http.connected() && (len < 0 || got < (size_t)len) && millis() - t0 < 30000) {
    const size_t avail = s->available();
    if (!avail) {
      delay(5);
      continue;
    }
    if (got + avail > b.n) break;
    got += s->readBytes(b.p + got, avail);
  }
  http.end();
  if (len > 0 && got != (size_t)len) {
    *why = "short download of " + url;
    return false;
  }
  const std::string dir = path.substr(0, path.rfind('/'));
  mkdirs(dir);
  if (!writeFile(path, b.p, got)) {
    *why = "cannot write " + path;
    return false;
  }
  return true;
}

bool sha256Matches(const std::string &path, const std::string &want) {
  if (want.empty()) return true;
  Buf b;
  if (!readFile(path, &b)) return false;
  uint8_t h[32];
  mbedtls_sha256(b.p, b.n, h, 0);
  return hex(h, 32) == want;
}

// A file from the site, from the cache when it is there (and matches).
bool ensure(const std::string &url, const std::string &sha, std::string *path, std::string *why) {
  *path = cachePath(url);
  if (exists(*path) && sha256Matches(*path, sha)) return true;
  runner::setPhase("Downloading " + url.substr(url.rfind('/', url.rfind('/') - 1) + 1));
  if (!download(url, *path, why)) return false;
  if (!sha256Matches(*path, sha)) {
    remove(path->c_str());
    *why = "checksum mismatch for " + url;
    return false;
  }
  return true;
}

std::string manifestPath() { return cachePath(std::string(kSite) + "/releases.json"); }
std::string pluginManifestPath() { return cachePath(std::string(kUsbPlugin) + "/releases.json"); }

// Fresh manifests when online; the cached ones otherwise.
bool manifests(std::string *why) {
  if (online()) {
    std::string w;
    runner::setPhase("Checking images.onerom.org");
    if (!download(std::string(kSite) + "/releases.json", manifestPath(), &w) ||
        !download(std::string(kUsbPlugin) + "/releases.json", pluginManifestPath(), &w))
      runner::note("Could not refresh the release lists (%s); using the saved ones", w.c_str());
  }
  if (exists(manifestPath()) && exists(pluginManifestPath())) return true;
  *why = "No One ROM release list on the card yet: connect Wi-Fi once to download it";
  return false;
}

struct Pick {
  std::string version, url, sha256;
};

bool pickFirmware(const std::string &board, Pick *p) {
  std::string m;
  if (!readText(manifestPath(), &m)) return false;
  char out[512];
  ort_pick_firmware(m.data(), m.size(), board.c_str(), out, sizeof(out));
  p->version = value(out, "version");
  p->url = value(out, "url");
  return !p->version.empty();
}

bool pickPlugin(const std::string &fw, Pick *p) {
  std::string m;
  if (!readText(pluginManifestPath(), &m)) return false;
  unsigned a = 0, b = 0, c = 0;
  sscanf(fw.c_str(), "%u.%u.%u", &a, &b, &c);
  char out[512];
  ort_pick_plugin(m.data(), m.size(), kUsbPlugin, a, b, c, out, sizeof(out));
  p->version = value(out, "version");
  p->url = value(out, "url");
  p->sha256 = value(out, "sha256");
  return !p->version.empty();
}

// ---- building ----------------------------------------------------------------

std::string jsonStr(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if ((unsigned char)c < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

// The image's name as the One ROM records it: its path under burner/images.
std::string recordedName(const std::string &path) {
  const std::string pre = std::string(app::kImages) + "/";
  return path.compare(0, pre.size(), pre) == 0 ? path.substr(pre.size()) : app::baseName(path);
}

// Builds the flashable image for one slot. On success *rom holds the ROM
// data as the slot serves it (for checking it afterwards).
bool build(const Program &p, const std::string &board, const std::string &size, const Pick &fw,
           const std::string &fw_path, const Pick &plugin, Buf *image, std::string *why) {
  const ChipType ct = chipType(p.type);
  if (!ct.ok) {
    *why = "unknown chip type " + p.type;
    return false;
  }
  static const char *kLevel[3] = {"active_low", "active_high", "ignore"};
  static const char *kFit[3] = {"none", "duplicate", "pad"};
  std::string chip = "{\"file\":" + jsonStr(p.name.empty() ? recordedName(p.image) : p.name) +
                     ",\"type\":" + jsonStr(p.type);
  for (size_t i = 0; i < ct.config_lines.size() && i < 3; i++)
    chip += ",\"" + ct.config_lines[i] + "\":\"" + kLevel[p.cs[i] % 3] + "\"";
  if (p.fit > 0) chip += std::string(",\"size_handling\":\"") + kFit[p.fit % 3] + "\"";
  chip += "}";
  const std::string json = "{\"version\":1,\"description\":\"Programmed by T48 for Tab5\",\"chip_sets\":["
                           "{\"type\":\"single\",\"chips\":[{\"file\":" + jsonStr(plugin.url) +
                           ",\"type\":\"system_plugin\"}]},{\"type\":\"single\",\"chips\":[" + chip + "]}]}";
  unsigned a = 0, b = 0, c = 0;
  sscanf(fw.version.c_str(), "%u.%u.%u", &a, &b, &c);
  char err[512] = "";
  OrtBuilder *bld = ort_builder_new(json.data(), json.size(), a, b, c, err, sizeof(err));
  if (!bld) {
    *why = std::string("config: ") + err;
    return false;
  }
  bool ok = true;
  char files[2048] = "";
  ort_builder_files(bld, files, sizeof(files));
  for (const auto &l : split(files, '\n')) {
    if (l.empty()) continue;
    const size_t tab = l.find('\t');
    const size_t id = strtoul(l.c_str(), nullptr, 10);
    const std::string src = l.substr(tab + 1);
    // A URL is a cached download; anything else is the image (by its name).
    const std::string path = src.compare(0, 8, "https://") == 0 ? cachePath(src) : p.image;
    Buf data;
    if (!readFile(path, &data)) {
      *why = "cannot read " + path;
      ok = false;
      break;
    }
    if (ort_builder_add_file(bld, id, data.p, data.n, err, sizeof(err))) {
      *why = std::string(err);
      ok = false;
      break;
    }
  }
  uint8_t *meta = nullptr, *rom = nullptr;
  size_t meta_n = 0, rom_n = 0;
  if (ok && ort_builder_build(bld, board.c_str(), size.c_str(), &meta, &meta_n, &rom, &rom_n, err, sizeof(err))) {
    *why = std::string(err);
    ok = false;
  }
  ort_builder_free(bld);
  Buf base;
  if (ok && !readFile(fw_path, &base)) {
    *why = "cannot read " + fw_path;
    ok = false;
  }
  uint8_t *img = nullptr;
  size_t img_n = 0;
  if (ok && ort_assemble(base.p, base.n, meta, meta_n, rom, rom_n, &img, &img_n)) {
    *why = "the firmware does not fit its region";
    ok = false;
  }
  ort_free(meta, meta_n);
  ort_free(rom, rom_n);
  if (ok) {
    ok = image->resize(img_n);
    if (ok) memcpy(image->p, img, img_n);
  }
  ort_free(img, img_n);
  return ok;
}

// ---- flashing (in the bootloader) --------------------------------------------

bool flash(const Buf &img, std::string *why) {
  const uint8_t excl = 1;
  if (usbdev::picoboot(0x01, &excl, 1, nullptr, 0) || usbdev::picoboot(0x06, nullptr, 0, nullptr, 0)) {
    *why = "the bootloader refused exclusive access";
    return false;
  }
  const uint32_t total = (img.n + kChunk - 1) / kChunk * kChunk;
  Buf page(kChunk), back(kChunk);
  for (uint32_t off = 0; off < total;) {
    const uint32_t n = (total - off >= 65536 && off % 65536 == 0) ? 65536 : kChunk;
    const uint32_t a[2] = {kFlash + off, n};
    runner::setPhase("Erasing", (int)(off * 100 / total));
    if (usbdev::picoboot(0x03, a, 8, nullptr, 0)) {
      *why = "erase failed at " + std::to_string(off);
      return false;
    }
    off += n;
  }
  for (uint32_t off = 0; off < total; off += kChunk) {
    memset(page.p, 0xFF, kChunk);
    memcpy(page.p, img.p + off, img.n - off < kChunk ? img.n - off : kChunk);
    const uint32_t a[2] = {kFlash + off, kChunk};
    runner::setPhase("Writing", (int)(off * 100 / total));
    if (usbdev::picoboot(0x05, a, 8, page.p, kChunk)) {
      *why = "write failed at " + std::to_string(off);
      return false;
    }
  }
  for (uint32_t off = 0; off < total; off += kChunk) {
    runner::setPhase("Verifying", (int)(off * 100 / total));
    memset(page.p, 0xFF, kChunk);
    memcpy(page.p, img.p + off, img.n - off < kChunk ? img.n - off : kChunk);
    if (readMem(kFlash + off, back.p, kChunk) || memcmp(back.p, page.p, kChunk)) {
      char b[64];
      snprintf(b, sizeof(b), "flash differs from the image at 0x%06lX", (unsigned long)off);
      *why = b;
      return false;
    }
  }
  return true;
}

// The ROM data as the slot serves it: the file, repeated or padded to size.
bool servedRom(const Program &p, uint32_t size, Buf *out, std::string *why) {
  Buf f;
  if (!readFile(p.image, &f)) {
    *why = "cannot read " + p.image;
    return false;
  }
  if (!out->resize(size)) return false;
  if (f.n >= size) {
    memcpy(out->p, f.p, size);
  } else if (p.fit == 1 && f.n && size % f.n == 0) {
    for (uint32_t o = 0; o < size; o += f.n) memcpy(out->p + o, f.p, f.n);
  } else {
    memset(out->p, 0xFF, size);
    memcpy(out->p, f.p, f.n);
  }
  return true;
}

bool needsRunning(std::string *why) {
  if (!usbdev::oneRomAttached()) {
    *why = "No One ROM on the USB-A port";
    return false;
  }
  if (usbdev::oneRomBootloader()) {
    runner::note("The One ROM is in its bootloader; starting it");
    if (!reboot(false)) {
      *why = "it did not come back running";
      return false;
    }
    probe(true);
  }
  return true;
}

int fail(const std::string &why) {
  runner::note("%s", why.c_str());
  return 1;
}

}  // namespace

// ---- state -------------------------------------------------------------------

bool present() { return usbdev::oneRomAttached(); }
bool bootloader() { return usbdev::oneRomBootloader(); }
std::string serial() { return usbdev::oneRomSerial(); }
uint32_t generation() { return usbdev::oneRomGeneration(); }

Info info() {
  Lock l;
  return g_info_gen == usbdev::oneRomGeneration() ? g_info : Info();
}

std::string latestFirmware() {
  Lock l;
  return g_latest;
}

std::string boardLabel(const std::string &board) {
  std::string s;
  bool up = true;
  for (char c : board) {
    if (c == '-') {
      s += ' ';
      up = true;
    } else {
      s += up ? (char)toupper((unsigned char)c) : c;
      up = false;
    }
  }
  // "Fire 28 c" -> "Fire 28 C": the revision letter is upper case.
  if (s.size() > 2 && s[s.size() - 2] == ' ') s.back() = (char)toupper((unsigned char)s.back());
  return s;
}

// ---- notes -------------------------------------------------------------------

std::string notesPath(const std::string &serial) { return std::string(kDevices) + "/" + serial + ".txt"; }

bool loadNotes(const std::string &serial, Notes *n) {
  *n = Notes();
  std::string t;
  if (serial.empty() || !readText(notesPath(serial), &t)) return false;
  // Header lines "key: value" up to a line of dashes; the notes follow.
  const size_t sep = t.find("\n---");
  const std::string head = sep == std::string::npos ? t : t.substr(0, sep);
  if (sep != std::string::npos) {
    const size_t body = t.find('\n', sep + 1);
    n->notes = body == std::string::npos ? "" : t.substr(body + 1);
    while (!n->notes.empty() && (n->notes.back() == '\n' || n->notes.back() == '\r')) n->notes.pop_back();
  }
  for (auto l : split(head, '\n')) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    const size_t c = l.find(':');
    if (c == std::string::npos) continue;
    std::string k = l.substr(0, c), v = l.substr(c + 1);
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    if (k == "Name") n->name = v;
    else if (k == "Last seen") n->seen = v;
    else if (k == "Last programmed") n->programmed = v;
    else if (k == "Image") n->image = v;
    else if (k == "Chip type") n->type = v;
    else if (k == "Firmware") n->firmware = v;
    else if (k == "Board") n->board = v;
  }
  return true;
}

bool saveNotes(const std::string &serial, const Notes &n) {
  if (serial.empty()) return false;
  mkdirs(kDevices);
  std::string t = "One ROM " + serial + "\n";
  t += "Name: " + n.name + "\n";
  if (!n.seen.empty()) t += "Last seen: " + n.seen + "\n";
  if (!n.programmed.empty()) t += "Last programmed: " + n.programmed + "\n";
  if (!n.image.empty()) t += "Image: " + n.image + "\n";
  if (!n.type.empty()) t += "Chip type: " + n.type + "\n";
  if (!n.firmware.empty()) t += "Firmware: " + n.firmware + "\n";
  if (!n.board.empty()) t += "Board: " + n.board + "\n";
  t += "---\n" + n.notes + "\n";
  return writeFile(notesPath(serial), (const uint8_t *)t.data(), t.size());
}

std::string displayName(const std::string &serial) {
  Notes n;
  if (loadNotes(serial, &n) && !n.name.empty()) return n.name;
  return serial.size() > 8 ? "One ROM " + serial.substr(0, 8) : "One ROM " + serial;
}

// ---- chip types ----------------------------------------------------------------

std::vector<std::string> chipTypes(const std::string &board) {
  static char buf[4096];
  ort_chip_types(board.c_str(), buf, sizeof(buf));
  std::vector<std::string> out;
  for (const auto &l : split(buf, '\n'))
    if (!l.empty()) out.push_back(l);
  return out;
}

ChipType chipType(const std::string &name) {
  char buf[512];
  ChipType ct;
  if (!ort_chip_info(name.c_str(), buf, sizeof(buf))) return ct;
  ct.ok = true;
  ct.size = strtoul(value(buf, "size").c_str(), nullptr, 10);
  ct.pins = atoi(value(buf, "pins").c_str());
  for (const auto &l : split(buf, '\n')) {
    if (l.compare(0, 5, "line=") != 0) continue;
    const auto f = split(l.substr(5), ':');
    if (f.size() >= 2 && f[1] == "config") {
      ct.config_lines.push_back(f[0]);
      ct.may_ignore.push_back(f.size() > 2 && f[2] == "ignore");
    }
  }
  return ct;
}

// ---- jobs ------------------------------------------------------------------------

int jobProbe(bool quiet) { return probe(quiet); }

int jobCheckUpdates() {
  std::string why;
  if (!manifests(&why)) return fail(why);
  const Info in = info();
  Pick fw;
  if (!in.board.empty() && pickFirmware(in.board, &fw)) {
    {
      Lock l;
      g_latest = fw.version;
    }
    runner::note("Newest One ROM firmware for %s: %s (this one has %s)", boardLabel(in.board).c_str(),
                 fw.version.c_str(), in.version.empty() ? "unknown" : in.version.c_str());
  }
  return 0;
}

int jobProgram(const Program &p) {
  std::string why;
  if (!usbdev::oneRomAttached()) return fail("No One ROM on the USB-A port");
  const std::string ser = usbdev::oneRomSerial();
  if (info().board.empty()) probe(true);
  Info in = info();
  Notes notes;
  loadNotes(ser, &notes);
  if (in.board.empty() && !notes.board.empty()) {
    runner::note("No firmware recognised on it; it was a %s when last seen", boardLabel(notes.board).c_str());
    in.board = notes.board;
  }
  if (in.board.empty())
    return fail("Cannot tell which One ROM board this is (no firmware recognised on it, and never seen before)");

  if (!manifests(&why)) return fail(why);
  Pick fw, plugin;
  if (!pickFirmware(in.board, &fw)) return fail("No firmware release lists " + in.board);
  if (!pickPlugin(fw.version, &plugin)) return fail("No USB plugin release runs on firmware " + fw.version);
  {
    Lock l;
    g_latest = fw.version;
  }
  std::string fw_path, plugin_path;
  if (!ensure(fw.url, "", &fw_path, &why) || !ensure(plugin.url, plugin.sha256, &plugin_path, &why))
    return fail(why);

  runner::setPhase("Building the image");
  Buf img;
  if (!build(p, in.board, in.size, fw, fw_path, plugin, &img, &why)) return fail("Build: " + why);
  runner::note("Image %s, CRC32 %08lX: firmware %s, USB plugin %s, %s as %s (%s)", app::bytesText(img.n).c_str(),
               (unsigned long)esp_rom_crc32_le(0, img.p, img.n), fw.version.c_str(), plugin.version.c_str(),
               app::baseName(p.image).c_str(), p.type.c_str(), boardLabel(in.board).c_str());

  if (!usbdev::oneRomBootloader()) {
    runner::setPhase("Stopping the One ROM");
    if (!reboot(true)) return fail("The One ROM did not come back in its bootloader");
  }
  if (!flash(img, &why)) {
    reboot(false);
    return fail(why);
  }
  runner::setPhase("Starting the One ROM");
  if (!reboot(false)) return fail("Flashed and verified, but it did not come back running");
  probe(true);
  in = info();
  if (in.version != fw.version) return fail("It runs firmware " + in.version + ", not " + fw.version);

  // What it serves now must be the image.
  const ChipType ct = chipType(p.type);
  Buf want, got(ct.size);
  if (!servedRom(p, ct.size, &want, &why)) return fail(why);
  runner::setPhase("Checking what it serves");
  if (readMem(kLive, got.p, ct.size)) return fail("Could not read back what it serves");
  if (memcmp(got.p, want.p, ct.size)) return fail("It serves something other than the image");

  notes.programmed = now();
  notes.seen = notes.programmed;
  notes.image = p.name.empty() ? recordedName(p.image) : p.name;
  notes.type = p.type;
  notes.firmware = fw.version;
  saveNotes(ser, notes);
  runner::note("Serving %s as %s, firmware %s: OK", app::baseName(p.image).c_str(), p.type.c_str(),
               fw.version.c_str());
  return 0;
}

int jobRead(const std::string &path, uint32_t size) {
  std::string why;
  if (!needsRunning(&why)) return fail(why);
  if (!size) return fail("Unknown ROM size");
  Buf b(size);
  runner::setPhase("Reading what it serves");
  if (readMem(kLive, b.p, size)) return fail("Read failed (does its firmware have the USB plugin?)");
  if (!writeFile(path, b.p, size)) return fail("Cannot write " + path);
  runner::note("Read %s from the One ROM into %s", app::bytesText(size).c_str(), app::baseName(path).c_str());
  return 0;
}

int jobVerify(const std::string &path) {
  std::string why;
  if (!needsRunning(&why)) return fail(why);
  Buf f, got;
  if (!readFile(path, &f)) return fail("Cannot read " + path);
  if (!got.resize(f.n)) return fail("No memory");
  runner::setPhase("Reading what it serves");
  if (readMem(kLive, got.p, f.n)) return fail("Read failed (does its firmware have the USB plugin?)");
  for (size_t i = 0; i < f.n; i++)
    if (got.p[i] != f.p[i]) {
      runner::note("Differs at 0x%06X: serves %02X, file has %02X", (unsigned)i, got.p[i], f.p[i]);
      return 1;
    }
  runner::note("The One ROM serves %s (%s): OK", app::baseName(path).c_str(), app::bytesText(f.n).c_str());
  return 0;
}

int jobUpdateFirmware() {
  std::string why;
  if (!needsRunning(&why)) return fail(why);
  probe(true);
  const Info in = info();
  std::vector<Slot> roms;
  for (const auto &s : in.slots)
    if (!s.plugin) roms.push_back(s);
  if (roms.size() != 1)
    return fail("This One ROM has " + std::to_string(roms.size()) +
                " ROM slots; keeping more than one is not supported. Use Program instead.");
  const Slot &s = roms[0];
  const ChipType ct = chipType(s.type);
  if (!ct.ok) return fail("Unknown chip type on the device: " + s.type);
  Program p;
  p.type = s.type;
  if (!ct.config_lines.empty()) {
    // Select-line polarities are not readable back; use the ones last used
    // for this type here.
    const auto &o = settings::get().onerom;
    if (o.type != s.type)
      return fail("A " + s.type + " needs its select lines set: choose it as the chip type and use Program");
    for (int i = 0; i < 3; i++) p.cs[i] = o.cs[i];
  }
  // Keep what it serves: read it first.
  const std::string keep = std::string(kCache) + "/keep-" + usbdev::oneRomSerial() + ".bin";
  mkdirs(kCache);
  Buf b(s.size);
  runner::setPhase("Saving what it serves");
  if (readMem(kLive, b.p, s.size) || !writeFile(keep, b.p, s.size))
    return fail("Could not save what it serves");
  runner::note("Saved its %s %s (%s) to keep it", s.type.c_str(), app::bytesText(s.size).c_str(),
               s.file.empty() ? "unnamed" : s.file.c_str());
  p.image = keep;
  p.name = s.file.empty() ? "kept.bin" : s.file;   // the device keeps the original name
  const int rc = jobProgram(p);
  if (rc == 0) {
    Notes n;
    loadNotes(usbdev::oneRomSerial(), &n);
    n.image = p.name;
    saveNotes(usbdev::oneRomSerial(), n);
  }
  return rc;
}

int jobIdentify() {
  std::string why;
  if (!needsRunning(&why)) return fail(why);
  // SET_LED (onerom_set_led_args_t): status LED, BEACON, which the firmware
  // bounds by hold_ms (offset 10) and then puts back what it interrupted.
  uint8_t a[16] = {0};
  a[0] = 0x00;   // status LED
  a[1] = 0x02;   // BEACON
  const uint32_t hold = 10000;
  memcpy(a + 10, &hold, 4);
  if (usbdev::picoboot(0x01, a, 16, nullptr, 0, usbdev::kOneRomMagic))
    return fail("Its firmware does not support LED control (update its firmware)");
  runner::note("%s: the status LED is flashing for 10 s", displayName(usbdev::oneRomSerial()).c_str());
  for (int i = 0; i < 10; i++) {
    runner::setPhase("Flashing its LED", i * 10);
    delay(1000);
  }
  return 0;
}

std::vector<std::pair<std::string, Notes>> known() {
  std::vector<std::pair<std::string, Notes>> out;
  DIR *d = opendir(kDevices);
  if (!d) return out;
  while (dirent *e = readdir(d)) {
    const std::string f = e->d_name;
    if (f.size() < 5 || f.compare(f.size() - 4, 4, ".txt") != 0) continue;
    const std::string ser = f.substr(0, f.size() - 4);
    Notes n;
    if (loadNotes(ser, &n)) out.push_back({ser, n});
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.second.seen > b.second.seen; });
  return out;
}

void tick() {
  const uint32_t gen = usbdev::oneRomGeneration();
  if (gen == g_seen_gen || !usbdev::oneRomAttached() || runner::busy()) return;
  g_seen_gen = gen;
  const std::string ser = usbdev::oneRomSerial();
  Notes n;
  const bool known = loadNotes(ser, &n);
  const std::string t = now();
  if (!t.empty() || !known) {
    if (!t.empty()) n.seen = t;
    saveNotes(ser, n);
  }
  runner::startFn([]() {
    const int rc = probe(false);
    // The newest firmware, for the screen (the saved list when offline).
    if (rc == 0 && exists(manifestPath())) {
      Pick fw;
      const Info in = info();
      if (!in.board.empty() && pickFirmware(in.board, &fw)) {
        Lock l;
        g_latest = fw.version;
      }
    }
    return rc;
  }, "One ROM");
}

}  // namespace onerom
}  // namespace burner
