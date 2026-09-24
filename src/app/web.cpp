#include "web.h"

#include <esp_heap_caps.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <dirent.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#include "app.h"
#include "parts.h"
#include "runner.h"
#include "settings.h"
#include "web_page.h"

#include "web_setup.h"

namespace burner {
namespace web {
namespace {

// The browser sees paths under the card's root ("/burner/images/x.bin") and
// may only reach /burner and below.
constexpr const char *kCard = "/sdcard";
constexpr const char *kTop = "/burner";

State g_state = State::Off;
std::string g_ssid;
uint32_t g_join_ms = 0;
httpd_handle_t g_server = nullptr;
SemaphoreHandle_t g_mux;
std::string g_chosen;

// The setup hotspot. Up only while there is no network to be on.
bool g_portal = false;
std::string g_ap;
DNSServer g_dns;
uint32_t g_portal_close_ms = 0;        // when joined: close the hotspot at this time
std::vector<Net> g_nets;               // the portal's last scan
bool g_want_scan = false;
std::string g_pending_ssid, g_pending_pass;
bool g_pending_join = false;
const IPAddress kApIp(192, 168, 4, 1);

std::vector<Net> doScan();

std::string query(httpd_req_t *req, const char *key) {
  char q[512];
  char v[400];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return "";
  if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return "";
  // Percent-decode.
  std::string out;
  for (size_t i = 0; v[i]; i++) {
    if (v[i] == '%' && isxdigit((unsigned char)v[i + 1]) && isxdigit((unsigned char)v[i + 2])) {
      char h[3] = {v[i + 1], v[i + 2], 0};
      out += (char)strtol(h, nullptr, 16);
      i += 2;
    } else if (v[i] == '+') {
      out += ' ';
    } else {
      out += v[i];
    }
  }
  return out;
}

// The card path for a browser path, or "" if it is outside /burner.
std::string cardPath(const std::string &p) {
  if (p.compare(0, strlen(kTop), kTop) != 0) return "";
  if (p.size() > strlen(kTop) && p[strlen(kTop)] != '/') return "";
  if (p.find("..") != std::string::npos) return "";
  return std::string(kCard) + p;
}

esp_err_t fail(httpd_req_t *req, const char *status, const char *why) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, why);
}

esp_err_t ok(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "ok");
}

std::string jsonStr(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if ((unsigned char)c < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

esp_err_t handlePage(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kWebPage, sizeof(kWebPage) - 1);
}

esp_err_t handleState(httpd_req_t *req) {
  const auto st = runner::status();
  const int p = app::part();
  std::string j = "{\"part\":" + jsonStr(p >= 0 ? parts::row(p).name : "") +
                  ",\"image\":" + jsonStr(app::image().path) + ",\"t48\":" + (app::status().t48 ? "true" : "false") +
                  ",\"busy\":" + (st.busy ? "true" : "false") + ",\"job\":" + jsonStr(st.title) + "}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, j.c_str());
}

esp_err_t handleList(httpd_req_t *req) {
  const std::string path = cardPath(query(req, "path"));
  if (path.empty()) return fail(req, "403 Forbidden", "outside /burner");
  mkdir((std::string(kCard) + kTop).c_str(), 0777);
  mkdir(app::kImages, 0777);
  DIR *d = opendir(path.c_str());
  if (!d) return fail(req, "404 Not Found", "no such folder");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "[");
  bool first = true;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    struct stat st;
    if (stat((path + "/" + e->d_name).c_str(), &st) != 0) continue;
    char b[64];
    snprintf(b, sizeof(b), ",\"dir\":%s,\"size\":%ld}", S_ISDIR(st.st_mode) ? "true" : "false", (long)st.st_size);
    std::string item = std::string(first ? "" : ",") + "{\"name\":" + jsonStr(e->d_name) + b;
    httpd_resp_sendstr_chunk(req, item.c_str());
    first = false;
  }
  closedir(d);
  httpd_resp_sendstr_chunk(req, "]");
  return httpd_resp_sendstr_chunk(req, nullptr);
}

esp_err_t handleGet(httpd_req_t *req) {
  const std::string path = cardPath(query(req, "path"));
  if (path.empty()) return fail(req, "403 Forbidden", "outside /burner");
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return fail(req, "404 Not Found", "no such file");
  httpd_resp_set_type(req, "application/octet-stream");
  static char *buf = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
  size_t n;
  while ((n = fread(buf, 1, 8192, f)) > 0) {
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// The body is the file itself (no multipart), streamed straight to the card.
esp_err_t handlePut(httpd_req_t *req) {
  const std::string path = cardPath(query(req, "path"));
  if (path.empty()) return fail(req, "403 Forbidden", "outside /burner");
  const std::string tmp = path + ".part";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return fail(req, "500 Internal Server Error", "cannot create the file");
  static char *buf = (char *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
  int left = req->content_len;
  while (left > 0) {
    const int n = httpd_req_recv(req, buf, left < 16384 ? left : 16384);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0 || fwrite(buf, 1, n, f) != (size_t)n) {
      fclose(f);
      remove(tmp.c_str());
      return fail(req, "500 Internal Server Error", "upload interrupted");
    }
    left -= n;
  }
  fclose(f);
  remove(path.c_str());
  if (rename(tmp.c_str(), path.c_str()) != 0) return fail(req, "500 Internal Server Error", "rename failed");
  runner::note("Wi-Fi: received %s (%d bytes)", app::baseName(path).c_str(), (int)req->content_len);
  return ok(req);
}

esp_err_t handleDelete(httpd_req_t *req) {
  const std::string path = cardPath(query(req, "path"));
  if (path.empty() || path == std::string(kCard) + kTop) return fail(req, "403 Forbidden", "not allowed");
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return fail(req, "404 Not Found", "no such file");
  const int r = S_ISDIR(st.st_mode) ? rmdir(path.c_str()) : remove(path.c_str());
  if (r != 0) return fail(req, "409 Conflict", S_ISDIR(st.st_mode) ? "folder is not empty" : "delete failed");
  return ok(req);
}

esp_err_t handleMkdir(httpd_req_t *req) {
  const std::string path = cardPath(query(req, "path"));
  if (path.empty()) return fail(req, "403 Forbidden", "outside /burner");
  if (mkdir(path.c_str(), 0777) != 0) return fail(req, "409 Conflict", "could not make the folder");
  return ok(req);
}

esp_err_t handleRename(httpd_req_t *req) {
  const std::string from = cardPath(query(req, "from")), to = cardPath(query(req, "to"));
  if (from.empty() || to.empty()) return fail(req, "403 Forbidden", "outside /burner");
  if (rename(from.c_str(), to.c_str()) != 0) return fail(req, "409 Conflict", "rename failed");
  return ok(req);
}

esp_err_t handleUse(httpd_req_t *req) {
  const std::string path = cardPath(query(req, "path"));
  if (path.empty()) return fail(req, "403 Forbidden", "outside /burner");
  if (runner::busy()) return fail(req, "409 Conflict", "the programmer is busy");
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_chosen = path;
  xSemaphoreGive(g_mux);
  return ok(req);
}

// ---- captive portal --------------------------------------------------------

esp_err_t redirectToSetup(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_sendstr(req, "<a href=\"http://192.168.4.1/setup\">Wi-Fi setup</a>");
}

// Phones probe a known URL to spot a captive portal (Apple
// /hotspot-detect.html, Android /generate_204, Windows /connecttest.txt).
// With DNS answering every name with 192.168.4.1, those land here as
// unknown paths; a redirect is what makes the phone open the setup page.
esp_err_t handleNotFound(httpd_req_t *req, httpd_err_code_t) {
  if (g_portal) return redirectToSetup(req);
  return fail(req, "404 Not Found", "not found");
}

esp_err_t handleRoot(httpd_req_t *req) {
  if (g_portal && g_state != State::Connected) return redirectToSetup(req);
  return handlePage(req);
}

esp_err_t handleSetup(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kSetupPage, sizeof(kSetupPage) - 1);
}

esp_err_t handleNets(httpd_req_t *req) {
  if (query(req, "rescan") == "1") {
    xSemaphoreTake(g_mux, portMAX_DELAY);
    g_want_scan = true;   // the main loop owns the radio
    xSemaphoreGive(g_mux);
    for (int i = 0; i < 100 && g_want_scan; i++) vTaskDelay(pdMS_TO_TICKS(100));
  }
  xSemaphoreTake(g_mux, portMAX_DELAY);
  std::string j = "[";
  for (size_t i = 0; i < g_nets.size(); i++) {
    char b[64];
    snprintf(b, sizeof(b), ",\"rssi\":%d,\"open\":%s}", g_nets[i].rssi, g_nets[i].open ? "true" : "false");
    j += std::string(i ? "," : "") + "{\"ssid\":" + jsonStr(g_nets[i].ssid) + b;
  }
  xSemaphoreGive(g_mux);
  j += "]";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, j.c_str());
}

std::string formValue(const std::string &body, const char *key) {
  const std::string k = std::string(key) + "=";
  size_t p = 0;
  while (p < body.size()) {
    size_t e = body.find('&', p);
    if (e == std::string::npos) e = body.size();
    if (body.compare(p, k.size(), k) == 0) {
      std::string v, raw = body.substr(p + k.size(), e - p - k.size());
      for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '+') v += ' ';
        else if (raw[i] == '%' && i + 2 < raw.size()) {
          v += (char)strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16);
          i += 2;
        } else v += raw[i];
      }
      return v;
    }
    p = e + 1;
  }
  return "";
}

esp_err_t handleWifi(httpd_req_t *req) {
  const int n = req->content_len;
  if (n <= 0 || n > 512) return fail(req, "400 Bad Request", "bad form");
  std::string body(n, '\0');
  int got = 0;
  while (got < n) {
    const int r = httpd_req_recv(req, &body[got], n - got);
    if (r <= 0) return fail(req, "400 Bad Request", "bad form");
    got += r;
  }
  const std::string ssid = formValue(body, "ssid");
  if (ssid.empty()) return fail(req, "400 Bad Request", "no network name");
  xSemaphoreTake(g_mux, portMAX_DELAY);
  g_pending_ssid = ssid;
  g_pending_pass = formValue(body, "pass");
  g_pending_join = true;
  xSemaphoreGive(g_mux);
  return ok(req);
}

esp_err_t handleWifiState(httpd_req_t *req) {
  const char *st = g_state == State::Connected ? "connected" : g_state == State::Connecting ? "joining"
                   : g_state == State::Failed ? "failed" : "off";
  std::string j = std::string("{\"state\":\"") + st + "\",\"ssid\":" + jsonStr(g_ssid) +
                  ",\"ip\":" + jsonStr(ip()) + ",\"portal\":" + (g_portal ? "true" : "false") + "}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, j.c_str());
}

void startServer() {
  if (g_server) return;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 8192;
  cfg.max_uri_handlers = 20;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 20;
  cfg.send_wait_timeout = 20;
  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    runner::note("Wi-Fi: web server failed to start");
    g_server = nullptr;
    return;
  }
  const struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*fn)(httpd_req_t *);
  } routes[] = {
      {"/", HTTP_GET, handleRoot},           {"/files", HTTP_GET, handlePage},
      {"/setup", HTTP_GET, handleSetup},     {"/api/nets", HTTP_GET, handleNets},
      {"/api/wifi", HTTP_POST, handleWifi},  {"/api/wifistate", HTTP_GET, handleWifiState},
      {"/api/state", HTTP_GET, handleState},
      {"/api/list", HTTP_GET, handleList},   {"/api/get", HTTP_GET, handleGet},
      {"/api/put", HTTP_PUT, handlePut},     {"/api/delete", HTTP_POST, handleDelete},
      {"/api/mkdir", HTTP_POST, handleMkdir}, {"/api/rename", HTTP_POST, handleRename},
      {"/api/use", HTTP_POST, handleUse},
  };
  for (auto &r : routes) {
    httpd_uri_t u = {};
    u.uri = r.uri;
    u.method = r.method;
    u.handler = r.fn;
    httpd_register_uri_handler(g_server, &u);
  }
  httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, handleNotFound);
}

void startPortal() {
  if (g_portal) return;
  // AP+STA: the station side scans, and joins once a network is chosen.
  WiFi.disconnect(false);   // stop retrying a network that is not there
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(g_ap.c_str());
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", kApIp);
  startServer();
  g_portal = true;
  g_portal_close_ms = 0;
  g_nets = doScan();
  runner::note("Wi-Fi setup: join the open network %s from a phone (N shows a QR code)", g_ap.c_str());
}

void stopPortal() {
  if (!g_portal) return;
  g_dns.stop();
  WiFi.softAPdisconnect(false);   // the hotspot only; the radio cannot be restarted
  WiFi.mode(WIFI_STA);
  g_portal = false;
  runner::note("Wi-Fi setup hotspot closed");
}

void connect(const std::string &ssid, const std::string &pass) {
  g_ssid = ssid;
  g_state = State::Connecting;
  g_join_ms = millis();
  WiFi.begin(ssid.c_str(), pass.c_str());
}

}  // namespace

void begin() {
  g_mux = xSemaphoreCreateMutex();
  WiFi.mode(WIFI_STA);
  // "T48-for-Tab5-" and two bytes of the P4's factory MAC, so two Tab5s
  // differ. (The Wi-Fi MAC comes from the C6 and read as zeros here.)
  const uint64_t mac = ESP.getEfuseMac();
  char ap[32];
  snprintf(ap, sizeof(ap), "T48-for-Tab5-%02X%02X", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  g_ap = ap;
  WiFi.setHostname("t48-for-tab5");
  auto &s = settings::get();
  if (s.wifi_ssid.empty()) {
    startPortal();   // nothing saved: set it up from a phone
    return;
  }
  connect(s.wifi_ssid, s.wifi_pass);
}

void loop() {
  if (g_portal) g_dns.processNextRequest();

  // Requests from the setup page, applied here: this loop owns the radio.
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const bool want_join = g_pending_join, want_scan = g_want_scan;
  const std::string ps = g_pending_ssid, pp = g_pending_pass;
  g_pending_join = false;
  xSemaphoreGive(g_mux);
  if (want_scan) {
    auto n = doScan();
    xSemaphoreTake(g_mux, portMAX_DELAY);
    g_nets = n;
    g_want_scan = false;
    xSemaphoreGive(g_mux);
  }
  if (want_join) join(ps, pp);

  if (g_state == State::Connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      g_state = State::Connected;
      startServer();
      runner::note("Wi-Fi: joined %s, files at http://%s/", g_ssid.c_str(), WiFi.localIP().toString().c_str());
      // Leave the hotspot up long enough for the phone to show the result.
      if (g_portal) g_portal_close_ms = millis() + 60000;
    } else if (millis() - g_join_ms > 20000) {
      g_state = State::Failed;
      runner::note("Wi-Fi: could not join %s", g_ssid.c_str());
      startPortal();
    }
  }
  if (g_portal && g_portal_close_ms && (int32_t)(millis() - g_portal_close_ms) >= 0) stopPortal(); else if (g_state == State::Connected && WiFi.status() != WL_CONNECTED) {
    // Dropped: the stack reconnects by itself; show it as joining meanwhile.
    g_state = State::Connecting;
    g_join_ms = millis();
  }
}

State state() { return g_state; }
std::string ssid() { return g_ssid; }
std::string ip() { return g_state == State::Connected ? WiFi.localIP().toString().c_str() : ""; }

std::string statusText() {
  if (g_portal && g_state != State::Connected) return "setup: " + g_ap;
  switch (g_state) {
    case State::Off: return "off";
    case State::Connecting: return "joining...";
    case State::Connected: return ip();
    case State::Failed: return "failed";
  }
  return "";
}

void join(const std::string &ssid, const std::string &pass) {
  auto &s = settings::get();
  s.wifi_ssid = ssid;
  s.wifi_pass = pass;
  settings::save();
  if (g_state == State::Off && !g_portal) WiFi.mode(WIFI_STA);
  else WiFi.disconnect(false);   // not the radio: it cannot be restarted
  connect(ssid, pass);
}

void forget() {
  auto &s = settings::get();
  s.wifi_ssid.clear();
  s.wifi_pass.clear();
  settings::save();
}

bool portalActive() { return g_portal; }
std::string portalSsid() { return g_ap; }
void setupFromPhone() { startPortal(); }

std::vector<Net> scan() { return doScan(); }

namespace {
std::vector<Net> doScan() {
  std::vector<Net> out;
  if (g_state == State::Off && !g_portal) WiFi.mode(WIFI_STA);
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    const std::string s = WiFi.SSID(i).c_str();
    if (s.empty()) continue;
    bool dup = false;
    for (auto &o : out) dup |= o.ssid == s;
    if (!dup) out.push_back({s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(out.begin(), out.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  return out;
}
}  // namespace

bool takeChosenImage(std::string *path) {
  xSemaphoreTake(g_mux, portMAX_DELAY);
  const bool any = !g_chosen.empty();
  *path = g_chosen;
  g_chosen.clear();
  xSemaphoreGive(g_mux);
  return any;
}

}  // namespace web
}  // namespace burner
