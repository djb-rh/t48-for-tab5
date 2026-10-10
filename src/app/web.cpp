#include "web.h"

#include <esp_heap_caps.h>
#include <DNSServer.h>
#include <ESP_HostedOTA.h>
#include <WiFi.h>
#include <esp32-hal-hosted.h>
#include <esp_attr.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>
#include <dirent.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#include "app.h"
#include "parts.h"
#include "onerom.h"
#include "runner.h"
#include "settings.h"
#include "web_page.h"
#include "zipx.h"

#include "web_setup.h"

namespace burner {
namespace web {
namespace {

// The browser sees paths under the card's root ("/burner/images/x.bin") and
// may only reach /burner and below.
constexpr const char *kCard = "/sdcard";
constexpr const char *kTop = "/burner";

State g_state = State::Off;
bool g_ever_connected = false;   // joined the saved network at least once this boot
uint32_t g_upload_pace_ms = 0;   // pause per 16 KB received (see handlePut)
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

// ---- link watchdog ------------------------------------------------------------
//
// The link to the router can die while the station still reports itself
// connected (seen after scans, during uploads and while idle), and nothing
// on the P4 notices. So the router is pinged every 10 s; 30 s of silence
// means restart (only when a restart loses nothing: the rejoin that was tried
// first never helped). The counters live in RAM that survives a software
// restart, so /api/wifistate can say what happened while nobody watched.
struct WatchCounters {
  uint32_t magic;
  uint32_t pings_ok, pings_lost, rejoins, restarts;
};
__NOINIT_ATTR WatchCounters g_wc;
constexpr uint32_t kWcMagic = 0x57A7C4E1;

esp_ping_handle_t g_ping = nullptr;
volatile uint32_t g_ping_replies = 0;
volatile bool g_ping_done = false;
bool g_router_answers = false;     // armed only once the router has answered
uint32_t g_ping_ms = 0;
int g_silent_checks = 0;
uint32_t g_rejoin_ms = 0;          // when the last rejoin started (0: none pending)

void onPingReply(esp_ping_handle_t, void *) { g_ping_replies++; }
void onPingEnd(esp_ping_handle_t, void *) { g_ping_done = true; }

void pingRouter() {
  const IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress(0, 0, 0, 0)) return;
  esp_ping_config_t c = ESP_PING_DEFAULT_CONFIG();
  IP_ADDR4(&c.target_addr, gw[0], gw[1], gw[2], gw[3]);
  c.count = 3;
  c.interval_ms = 300;
  c.timeout_ms = 1000;
  c.task_stack_size = 3072;
  esp_ping_callbacks_t cb = {};
  cb.on_ping_success = onPingReply;
  cb.on_ping_end = onPingEnd;
  g_ping_replies = 0;
  g_ping_done = false;
  if (esp_ping_new_session(&c, &cb, &g_ping) != ESP_OK) {
    g_ping = nullptr;
    return;
  }
  esp_ping_start(g_ping);
}

}  // namespace
void reconnect();
namespace {
void watchLink() {
  if (g_state != State::Connected) {
    g_silent_checks = 0;
    return;
  }
  if (g_ping) {
    if (!g_ping_done) return;
    const bool answered = g_ping_replies > 0;
    esp_ping_delete_session(g_ping);
    g_ping = nullptr;
    if (answered) {
      g_wc.pings_ok++;
      g_router_answers = true;
      g_silent_checks = 0;
      g_rejoin_ms = 0;
      return;
    }
    g_wc.pings_lost++;
    if (!g_router_answers) return;   // a router that never answers pings: no watchdog
    if (++g_silent_checks < 3) return;
    g_silent_checks = 0;
    // Rejoining was tried first and never once brought a dead link back
    // (the C6 or its SDIO link is wedged); a restart, which also resets the
    // C6, always did. Restart only when that loses nothing; otherwise keep
    // checking and restart when the user is back on the main screen.
    if (app::idleOnMain()) {
      g_wc.restarts++;
      runner::note("Wi-Fi: the link to the router went silent; restarting to recover");
      delay(500);
      ESP.restart();
    } else if (!g_rejoin_ms) {
      g_rejoin_ms = millis();
      runner::note("Wi-Fi: the link went silent; it recovers on the main screen");
    }
    return;
  }
  if (millis() - g_ping_ms >= 10000) {
    g_ping_ms = millis();
    pingRouter();
  }
}

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

// ---- One ROM names and notes -------------------------------------------------

std::string jsonText(const std::string &v) {
  std::string o = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (c == '\n') o += "\\n";
    else if ((unsigned char)c >= 0x20) o += c;
  }
  return o + "\"";
}

esp_err_t handleOneRomList(httpd_req_t *req) {
  std::string j = "{\"connected\":" + jsonText(onerom::serial()) + ",\"devices\":[";
  bool first = true;
  for (const auto &d : onerom::known()) {
    const auto &n = d.second;
    j += std::string(first ? "" : ",") + "{\"serial\":" + jsonText(d.first) + ",\"name\":" + jsonText(n.name) +
         ",\"notes\":" + jsonText(n.notes) + ",\"seen\":" + jsonText(n.seen) + ",\"programmed\":" +
         jsonText(n.programmed) + ",\"image\":" + jsonText(n.image) + ",\"type\":" + jsonText(n.type) +
         ",\"firmware\":" + jsonText(n.firmware) + ",\"board\":" + jsonText(onerom::boardLabel(n.board)) + "}";
    first = false;
  }
  j += "]}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, j.c_str());
}

// POST /api/onerom?serial=X, body: the name on the first line, the notes after.
esp_err_t handleOneRomSave(httpd_req_t *req) {
  const std::string serial = query(req, "serial");
  bool hex = !serial.empty() && serial.size() <= 32;
  for (char c : serial) hex = hex && isxdigit((unsigned char)c);
  if (!hex) return fail(req, "400 Bad Request", "bad serial");
  if (req->content_len > 4096) return fail(req, "413 Payload Too Large", "notes are limited to 4 KB");
  std::string body(req->content_len, '\0');
  size_t got = 0;
  while (got < body.size()) {
    const int n = httpd_req_recv(req, &body[got], body.size() - got);
    if (n <= 0) return fail(req, "500 Internal Server Error", "upload interrupted");
    got += n;
  }
  body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
  const size_t nl = body.find('\n');
  onerom::Notes n;
  onerom::loadNotes(serial, &n);
  n.name = body.substr(0, nl);
  n.notes = nl == std::string::npos ? "" : body.substr(nl + 1);
  if (!onerom::saveNotes(serial, n)) return fail(req, "500 Internal Server Error", "could not save");
  return ok(req);
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
  // ?view=1: shown in the browser (README files), not downloaded.
  httpd_resp_set_type(req, query(req, "view") == "1" ? "text/plain; charset=utf-8" : "application/octet-stream");
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
  // The page sends a file in 16 KB pieces (?offset=&total=): one long inbound
  // stream is what wedges the C6's SDIO link (espressif/esp-hosted-mcu#184;
  // FlapBoard went from wedging to 12/12 clean 2 MB uploads this way). Each
  // piece appends to the .part file; the last one finishes it. Without the
  // two parameters the body is the whole file, as before.
  const std::string off_s = query(req, "offset"), total_s = query(req, "total");
  const bool pieces = !off_s.empty() && !total_s.empty();
  const long offset = pieces ? atol(off_s.c_str()) : 0;
  const long total = pieces ? atol(total_s.c_str()) : (long)req->content_len;
  if (pieces && offset > 0) {
    struct stat st;
    if (stat(tmp.c_str(), &st) != 0 || st.st_size != offset)
      return fail(req, "409 Conflict", "upload out of step; start it again");
  }
  FILE *f = fopen(tmp.c_str(), offset > 0 ? "ab" : "wb");
  if (!f) return fail(req, "500 Internal Server Error", "cannot create the file");
  static char *buf = (char *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
  int left = req->content_len;
  int timeouts = 0;
  while (left > 0) {
    const int n = httpd_req_recv(req, buf, left < 16384 ? left : 16384);
    // A stalled sender (or a dead link) must end the upload: retrying a
    // timeout forever held the file open at 0 bytes and the server with it.
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0 || fwrite(buf, 1, n, f) != (size_t)n) {
      fclose(f);
      remove(tmp.c_str());
      return fail(req, "500 Internal Server Error", "upload interrupted");
    }
    left -= n;
    timeouts = 0;
    // Pacing: a continuous inbound stream over the C6's SDIO link is a known
    // way to wedge it (espressif/esp-hosted-mcu#184); a pause per chunk
    // gives it room. 0 = none.
    if (g_upload_pace_ms) vTaskDelay(pdMS_TO_TICKS(g_upload_pace_ms));
  }
  fclose(f);
  if (offset + (long)req->content_len < total) return ok(req);   // more pieces to come
  remove(path.c_str());
  if (rename(tmp.c_str(), path.c_str()) != 0) return fail(req, "500 Internal Server Error", "rename failed");
  runner::note("Wi-Fi: received %s (%ld bytes)", app::baseName(path).c_str(), total);
  // A zip is a way of carrying files, not an image: unpack it into a folder
  // named after it, and drop the zip once everything is out.
  if (zipx::isZip(path)) {
    const std::string dest = path.substr(0, path.rfind('/') + 1) + zipx::stem(app::baseName(path));
    const auto r = zipx::extract(path, dest);
    char msg[200];
    if (r.ok) {
      remove(path.c_str());
      snprintf(msg, sizeof(msg), "Extracted %d files into %s/", r.files, app::baseName(dest).c_str());
    } else {
      snprintf(msg, sizeof(msg), "Uploaded, but extracting failed: %s", r.error.c_str());
    }
    runner::note("Wi-Fi: %s", msg);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
  }
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

// Test knob for the upload pacing experiment (see handlePut).
esp_err_t handlePace(httpd_req_t *req) {
  g_upload_pace_ms = (uint32_t)atoi(query(req, "ms").c_str());
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
  char w[160];
  snprintf(w, sizeof(w), ",\"uptime_s\":%lu,\"pings_ok\":%lu,\"pings_lost\":%lu,\"rejoins\":%lu,\"restarts\":%lu",
           (unsigned long)(millis() / 1000), (unsigned long)g_wc.pings_ok, (unsigned long)g_wc.pings_lost,
           (unsigned long)g_wc.rejoins, (unsigned long)g_wc.restarts);
  std::string j = std::string("{\"state\":\"") + st + "\",\"ssid\":" + jsonStr(g_ssid) +
                  ",\"ip\":" + jsonStr(ip()) + ",\"portal\":" + (g_portal ? "true" : "false") + w + "}";
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
      {"/api/use", HTTP_POST, handleUse},    {"/api/pace", HTTP_POST, handlePace},
      {"/api/onerom", HTTP_GET, handleOneRomList}, {"/api/onerom", HTTP_POST, handleOneRomSave},
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
  // With a saved network the station keeps trying it (see loop): a router
  // that was slow or down at boot must not strand the Tab5 in setup mode.
  if (settings::get().wifi_ssid.empty()) WiFi.disconnect(false);
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
  // The default fast scan joins the first access point it hears; with more
  // than one on the network, scan every channel and take the strongest.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  g_ssid = ssid;
  g_state = State::Connecting;
  g_join_ms = millis();
  WiFi.begin(ssid.c_str(), pass.c_str());
  // No modem power saving: the C6 napping between beacons is a suspect in
  // the link dying while idle, and a bench tool has the milliamps to spare.
  WiFi.setSleep(false);
}

}  // namespace

void begin() {
  g_mux = xSemaphoreCreateMutex();
  // Counters survive a software restart; anything else (power on) clears them.
  if (g_wc.magic != kWcMagic || esp_reset_reason() == ESP_RST_POWERON) {
    memset(&g_wc, 0, sizeof(g_wc));
    g_wc.magic = kWcMagic;
  }
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
  {
    static int last = -1;
    const int now = (int)WiFi.status();
    if (now != last) {
      Serial.printf("wifi: status %d -> %d, rssi %d\n", last, now, (int)WiFi.RSSI());
      last = now;
    }
  }

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
      g_ever_connected = true;
      startServer();
      // The clock, for the One ROM notes' dates. US Eastern (the
      // workbench's); the card's files are stamped in it too.
      configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.google.com");
      runner::note("Wi-Fi: joined %s, files at http://%s/", g_ssid.c_str(), WiFi.localIP().toString().c_str());
      static bool reported = false;
      if (!reported) {
        reported = true;
        runner::note("Wi-Fi chip firmware %s, this build expects %s%s", coprocVersion().c_str(),
                     hostVersion().c_str(), coprocUpdateAvailable() ? " (update available: Wi-Fi screen)" : "");
      }
      // Leave the hotspot up long enough for the phone to show the result.
      if (g_portal) g_portal_close_ms = millis() + 60000;
    } else if (millis() - g_join_ms > 45000) {   // since the C6's 2.12 firmware a join can take ~25 s
      if (g_ever_connected && !g_portal) {
        // It worked before (router rebooting, out of range): keep trying the
        // saved network rather than stranding the Tab5 in setup mode.
        g_join_ms = millis();
        reconnect();
      } else {
        g_state = State::Failed;
        runner::note("Wi-Fi: could not join %s", g_ssid.c_str());
        startPortal();
      }
    }
  }
  if (g_state == State::Connected && WiFi.status() != WL_CONNECTED) {
    // Dropped: the stack reconnects by itself; show it as joining meanwhile.
    g_state = State::Connecting;
    g_join_ms = millis();
  }
  if (g_portal && g_portal_close_ms && (int32_t)(millis() - g_portal_close_ms) >= 0) stopPortal();
  // Setup mode with a saved network: try it again every 30 s, but not while a
  // phone is on the hotspot (joining moves the radio's channel under it).
  if (g_portal && g_state == State::Failed && !settings::get().wifi_ssid.empty() &&
      WiFi.softAPgetStationNum() == 0 && millis() - g_join_ms > 30000) {
    auto &st = settings::get();
    connect(st.wifi_ssid, st.wifi_pass);
  }
  watchLink();
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

void setUploadPace(uint32_t ms) { g_upload_pace_ms = ms; }

void reconnect() {
  auto &s = settings::get();
  if (s.wifi_ssid.empty()) return;
  WiFi.disconnect(false);   // not the radio: it cannot be restarted
  connect(s.wifi_ssid, s.wifi_pass);
}

void forget() {
  auto &s = settings::get();
  s.wifi_ssid.clear();
  s.wifi_pass.clear();
  settings::save();
}

std::string coprocVersion() {
  uint32_t a = 0, b = 0, c = 0;
  hostedHasUpdate();   // (re)reads the co-processor's version
  hostedGetSlaveVersion(&a, &b, &c);
  char t[24];
  snprintf(t, sizeof(t), "%lu.%lu.%lu", (unsigned long)a, (unsigned long)b, (unsigned long)c);
  return t;
}

std::string hostVersion() {
  uint32_t a = 0, b = 0, c = 0;
  hostedGetHostVersion(&a, &b, &c);
  char t[24];
  snprintf(t, sizeof(t), "%lu.%lu.%lu", (unsigned long)a, (unsigned long)b, (unsigned long)c);
  return t;
}

bool coprocUpdateAvailable() { return hostedHasUpdate(); }

bool updateCoproc() { return updateEspHostedSlave(); }

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
