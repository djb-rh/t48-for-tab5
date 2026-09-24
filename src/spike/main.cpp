// Phase 0 spike: can the Tab5's USB-A port drive an XGecu T48?
//
// The questions, in order:
//   1. Does the port's 5 V bring the T48 up (it has no other supply)?
//   2. Does it enumerate, and at which speed? The USB-A socket is on the
//      P4's high-speed PHY (USB2_OTG_D+/-), the T48 is a high-speed device.
//   3. Does it answer minipro's first command, "get system info"? That is a
//      5-byte bulk OUT on EP 0x01 and a bulk IN on EP 0x81 (minipro.c,
//      minipro_get_system_info()), and the reply's layout is documented there.
//   4. What does it cost in current? Logged from the INA226 once a second.
//
// Tap the screen for the full system info again. Serial commands: i = same, p = toggle root port power,
// u = toggle the USB-A 5 V rail, d = dump descriptors again.

#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

#include <cstdarg>
#include <cstring>

namespace {

// ---- a log that goes to serial and to the glass --------------------------
//
// The USB client task produces most of the lines; only loop() draws, so the
// display is touched from one task.
constexpr int kLines = 26;
constexpr int kCols = 96;
char g_lines[kLines][kCols];
int g_head = 0;      // next line to write
int g_count = 0;
bool g_dirty = true;
SemaphoreHandle_t g_log_mux;

void logf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  xSemaphoreTake(g_log_mux, portMAX_DELAY);
  strlcpy(g_lines[g_head], buf, kCols);
  g_head = (g_head + 1) % kLines;
  if (g_count < kLines) g_count++;
  g_dirty = true;
  xSemaphoreGive(g_log_mux);
}

char g_status[kCols] = "";

void drawLog() {
  xSemaphoreTake(g_log_mux, portMAX_DELAY);
  if (!g_dirty) {
    xSemaphoreGive(g_log_mux);
    return;
  }
  g_dirty = false;
  auto &d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setCursor(8, 8);
  d.print("tab5-burner spike: T48 on USB-A   ");
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.print(g_status);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  const int first = (g_head - g_count + kLines) % kLines;
  for (int i = 0; i < g_count; i++) {
    d.setCursor(8, 40 + i * 26);
    d.print(g_lines[(first + i) % kLines]);
  }
  d.endWrite();
  xSemaphoreGive(g_log_mux);
}

// ---- USB host --------------------------------------------------------------

constexpr uint16_t kVid = 0xA466, kPid = 0x0A53;  // TL866II+ / T48 / T56 share these

usb_host_client_handle_t g_client = nullptr;
usb_device_handle_t g_dev = nullptr;
uint8_t g_dev_addr = 0;
bool g_claimed = false;
uint16_t g_mps[16][2];   // [ep number][dir: 0 out, 1 in]
volatile uint8_t g_pending_addr = 0;
volatile bool g_gone = false;
volatile char g_cmd = 0;   // from loop() to the client task
bool g_port_on = false;
bool g_rail_on = false;

const char *speedName(usb_speed_t s) {
  switch (s) {
    case USB_SPEED_LOW: return "low (1.5M)";
    case USB_SPEED_FULL: return "full (12M)";
    case USB_SPEED_HIGH: return "HIGH (480M)";
    default: return "?";
  }
}

struct Wait {
  volatile bool done;
};

void onXfer(usb_transfer_t *t) { ((Wait *)t->context)->done = true; }

// One bulk transfer, synchronously. Only from the client task: completions
// arrive through the same event pump this spins.
esp_err_t bulk(uint8_t ep, uint8_t *buf, size_t len, int *got, uint32_t timeout_ms) {
  const bool in = ep & 0x80;
  const uint16_t mps = g_mps[ep & 0x0F][in ? 1 : 0];
  if (!mps) {
    logf("  EP %02x is not in the descriptor", ep);
    return ESP_ERR_INVALID_ARG;
  }
  // The stack wants an IN transfer sized to a whole number of packets.
  const size_t n = in ? usb_round_up_to_mps(len, mps) : len;
  usb_transfer_t *t = nullptr;
  esp_err_t e = usb_host_transfer_alloc(n, 0, &t);
  if (e != ESP_OK) return e;
  if (!in) memcpy(t->data_buffer, buf, len);
  t->num_bytes = n;
  t->device_handle = g_dev;
  t->bEndpointAddress = ep;
  t->callback = onXfer;
  t->timeout_ms = timeout_ms;
  Wait w{false};
  t->context = &w;
  e = usb_host_transfer_submit(t);
  if (e == ESP_OK) {
    const uint32_t t0 = millis();
    while (!w.done && millis() - t0 < timeout_ms) {
      usb_host_client_handle_events(g_client, pdMS_TO_TICKS(5));
    }
    if (!w.done) {
      logf("  EP %02x: no completion in %u ms, halting", ep, (unsigned)timeout_ms);
      usb_host_endpoint_halt(g_dev, ep);
      usb_host_endpoint_flush(g_dev, ep);
      while (!w.done) usb_host_client_handle_events(g_client, pdMS_TO_TICKS(5));
      usb_host_endpoint_clear(g_dev, ep);
      e = ESP_ERR_TIMEOUT;
    } else if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
      logf("  EP %02x: transfer status %d", ep, (int)t->status);
      e = ESP_FAIL;
    } else {
      *got = t->actual_num_bytes;
      if (in) memcpy(buf, t->data_buffer, *got < (int)len ? *got : len);
    }
  }
  usb_host_transfer_free(t);
  return e;
}

void hexdump(const uint8_t *b, int n) {
  for (int i = 0; i < n; i += 16) {
    char line[80];
    int o = snprintf(line, sizeof(line), "  %02x:", i);
    for (int k = i; k < i + 16 && k < n; k++) o += snprintf(line + o, sizeof(line) - o, " %02x", b[k]);
    logf("%s", line);
  }
}

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void printable(char *out, const uint8_t *in, int n) {
  int o = 0;
  for (int i = 0; i < n && in[i]; i++) out[o++] = (in[i] >= 0x20 && in[i] < 0x7F) ? in[i] : '.';
  out[o] = 0;
}

float g_t48_volts = 0;   // from the last system-info reply

// minipro_get_system_info(): five zero bytes out, up to 80 back. Quiet mode
// only refreshes the supply reading for the status line.
void systemInfo(bool verbose = true) {
  if (!g_claimed) {
    logf("sysinfo: no T48 open");
    return;
  }
  uint8_t msg[80] = {0};
  int got = 0;
  const uint32_t t0 = micros();
  esp_err_t e = bulk(0x01, msg, 5, &got, 1000);
  if (e != ESP_OK) {
    logf("sysinfo: send failed (%d)", (int)e);
    return;
  }
  memset(msg, 0, sizeof(msg));
  e = bulk(0x81, msg, sizeof(msg), &got, 2000);
  const uint32_t us = micros() - t0;
  if (e != ESP_OK) {
    logf("sysinfo: receive failed (%d)", (int)e);
    return;
  }
  if (!verbose) {
    if (msg[6] == 7) g_t48_volts = (le32(msg + 56) * 0xccf6 / 0x27000) / 100.0f;
    return;
  }
  logf("sysinfo: %d bytes back in %u us", got, (unsigned)us);
  hexdump(msg, got < 64 ? got : 64);
  if (msg[6] != 7) {
    logf("sysinfo: device type %u, not a T48 (7)", msg[6]);
    return;
  }
  char date[17], code[9], serial[25];
  printable(date, msg + 8, 16);
  printable(code, msg + 24, 8);
  printable(serial, msg + 32, 24);
  const uint32_t raw = le32(msg + 56);
  const float volts = (raw * 0xccf6 / 0x27000) / 100.0f;
  g_t48_volts = volts;
  logf("T48  firmware 00.%u.%02u  %s", msg[5], msg[4], msg[4] == 0 ? "(BOOTLOADER)" : "");
  logf("     made %s  code %s", date, code);
  logf("     serial %s", serial);
  logf("     supply %.2f V   link %s", volts, msg[60] ? "480 Mbps" : "12 Mbps (!)");
}

void dumpDescriptors() {
  const usb_device_desc_t *dd = nullptr;
  const usb_config_desc_t *cd = nullptr;
  usb_device_info_t info = {};
  usb_host_device_info(g_dev, &info);
  usb_host_get_device_descriptor(g_dev, &dd);
  usb_host_get_active_config_descriptor(g_dev, &cd);
  logf("device %04x:%04x bcdUSB %04x speed %s, ep0 %u, %u mA asked", dd->idVendor,
       dd->idProduct, dd->bcdUSB, speedName(info.speed), dd->bMaxPacketSize0,
       cd->bMaxPower * 2);
  auto str = [](const usb_str_desc_t *s, char *out, size_t cap) {
    size_t o = 0;
    if (s)
      for (int k = 0; k < (s->bLength - 2) / 2 && o + 1 < cap; k++) {
        const uint16_t c = s->wData[k];
        if (c >= 0x20 && c < 0x7F) out[o++] = (char)c;
      }
    out[o] = 0;
  };
  char m[48], p[48], sn[48];
  str(info.str_desc_manufacturer, m, sizeof(m));
  str(info.str_desc_product, p, sizeof(p));
  str(info.str_desc_serial_num, sn, sizeof(sn));
  logf("  \"%s\" / \"%s\" / \"%s\"", m, p, sn);
  memset(g_mps, 0, sizeof(g_mps));
  for (int i = 0; i < cd->bNumInterfaces; i++) {
    int off = 0;
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cd, i, 0, &off);
    if (!intf) continue;
    logf("  intf %u class %02x/%02x/%02x, %u endpoints", intf->bInterfaceNumber,
         intf->bInterfaceClass, intf->bInterfaceSubClass, intf->bInterfaceProtocol,
         intf->bNumEndpoints);
    for (int e = 0; e < intf->bNumEndpoints; e++) {
      int eoff = off;
      const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, e, cd->wTotalLength, &eoff);
      if (!ep) continue;
      static const char *kind[] = {"ctrl", "iso", "bulk", "intr"};
      const uint16_t mps = ep->wMaxPacketSize & 0x7FF;
      logf("    EP %02x %s mps %u", ep->bEndpointAddress, kind[ep->bmAttributes & 3], mps);
      if (intf->bInterfaceNumber == 0)
        g_mps[ep->bEndpointAddress & 0x0F][ep->bEndpointAddress & 0x80 ? 1 : 0] = mps;
    }
  }
}

void closeDevice() {
  if (!g_dev) return;
  if (g_claimed) usb_host_interface_release(g_client, g_dev, 0);
  usb_host_device_close(g_client, g_dev);
  g_dev = nullptr;
  g_claimed = false;
  strlcpy(g_status, "no T48", sizeof(g_status));
  logf("T48 gone");
}

void openDevice(uint8_t addr) {
  if (g_dev) return;
  esp_err_t e = usb_host_device_open(g_client, addr, &g_dev);
  if (e != ESP_OK) {
    logf("open addr %u failed (%d)", addr, (int)e);
    return;
  }
  g_dev_addr = addr;
  const usb_device_desc_t *dd = nullptr;
  usb_host_get_device_descriptor(g_dev, &dd);
  dumpDescriptors();
  if (dd->idVendor != kVid || dd->idProduct != kPid) {
    logf("not an XGecu programmer, leaving it alone");
    return;
  }
  e = usb_host_interface_claim(g_client, g_dev, 0, 0);
  if (e != ESP_OK) {
    logf("claim interface 0 failed (%d)", (int)e);
    return;
  }
  g_claimed = true;
  strlcpy(g_status, "T48 open", sizeof(g_status));
  systemInfo();
}

void onClientEvent(const usb_host_client_event_msg_t *msg, void *) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) g_pending_addr = msg->new_dev.address;
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) g_gone = true;
}

bool onEnumFilter(const usb_device_desc_t *dd, uint8_t *cfg) {
  Serial.printf("enumerating %04x:%04x\n", dd->idVendor, dd->idProduct);
  *cfg = 1;
  return true;
}

void hostTask(void *) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

void setPort(bool on) {
  const esp_err_t e = usb_host_lib_set_root_port_power(on);
  g_port_on = on;
  logf("root port power %s (%d)", on ? "on" : "off", (int)e);
}

void setRail(bool on) {
  M5.Power.setExtOutput(on, m5::ext_USB);
  g_rail_on = on;
  logf("USB-A 5V rail %s", on ? "on" : "off");
}

void clientTask(void *) {
  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = onClientEvent;
  if (usb_host_client_register(&cfg, &g_client) != ESP_OK) {
    logf("client register failed");
    vTaskDelete(nullptr);
    return;
  }
  // Power only once someone is listening, or a device present at boot is
  // announced to nobody (learned on the gamepad).
  setRail(true);
  delay(50);
  setPort(true);
  for (;;) {
    usb_host_client_handle_events(g_client, pdMS_TO_TICKS(50));
    if (g_gone) {
      g_gone = false;
      closeDevice();
    }
    if (g_pending_addr) {
      const uint8_t a = g_pending_addr;
      g_pending_addr = 0;
      openDevice(a);
    }
    // The T48's own reading of its supply, every two seconds, so it can be
    // watched on battery with no computer attached.
    static uint32_t polled = 0;
    if (g_claimed && millis() - polled > 2000) {
      polled = millis();
      systemInfo(false);
    }
    const char c = g_cmd;
    if (c) {
      g_cmd = 0;
      switch (c) {
        case 'i': systemInfo(); break;
        case 'd': if (g_dev) dumpDescriptors(); break;
        case 'p': if (!g_port_on) setPort(true); else { closeDevice(); setPort(false); } break;
        case 'u': setRail(!g_rail_on); break;
        default: break;
      }
    }
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;   // every outgoing 5 V rail; only USB-A is wanted
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never block on a USB-serial nobody reads
  g_log_mux = xSemaphoreCreateMutex();
  M5.Display.setRotation(3);
  M5.Display.setBrightness(180);
  M5.Display.setFont(&fonts::FreeMono12pt7b);
  M5.Display.setTextSize(1);
  strlcpy(g_status, "no T48", sizeof(g_status));

  logf("boot: psram %u KB, internal %u KB", (unsigned)(ESP.getFreePsram() / 1024),
       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  logf("power before USB-A: %d mA, %d mV", (int)M5.Power.getBatteryCurrent(),
       (int)M5.Power.getBatteryVoltage());

  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  hc.enum_filter_cb = onEnumFilter;
  hc.root_port_unpowered = true;
  const esp_err_t e = usb_host_install(&hc);
  if (e != ESP_OK) {
    logf("usb_host_install failed: %d", (int)e);
    return;
  }
  xTaskCreatePinnedToCore(hostTask, "usbhost", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(clientTask, "t48", 8192, nullptr, 4, nullptr, 0);
}

void loop() {
  M5.update();
  if (M5.Touch.getDetail().wasPressed()) g_cmd = 'i';
  while (Serial.available()) {
    const char c = Serial.read();
    if (c > ' ') g_cmd = c;
  }
  static uint32_t last = 0;
  if (millis() - last > 1000) {
    last = millis();
    const int ma = M5.Power.getBatteryCurrent();
    const int mv = M5.Power.getBatteryVoltage();
    if (g_claimed)
      snprintf(g_status, sizeof(g_status), "T48 %.2f V  bat %d mA %d mV", g_t48_volts, ma, mv);
    else
      snprintf(g_status, sizeof(g_status), "no T48  bat %d mA %d mV", ma, mv);
    Serial.printf("power: %d mA %d mV  t48 %.2f V\n", ma, mv, g_t48_volts);
    xSemaphoreTake(g_log_mux, portMAX_DELAY);
    g_dirty = true;
    xSemaphoreGive(g_log_mux);
  }
  drawLog();
  delay(20);
}
