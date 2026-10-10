// minipro's USB layer (usb.h) on the ESP32-P4's USB host stack, for the T48.
//
// minipro's own usb_nix.c does this with libusb. The T48 needs very little of
// it: bulk EP 01/81 for commands and replies, bulk EP 02/82 for payload. The
// two-endpoint split transfers in usb_nix.c are only reached when a caller
// passes a non-zero limit, and every T48 call passes 0 (t48.c).
//
// Threads: the client task below owns the device (open, claim, close) and
// pumps the stack's events, which is where transfer completions are
// delivered. minipro runs in a task of its own and blocks on a semaphore per
// transfer, so it never has to pump anything itself.

#include "usb_esp.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <usb/usb_host.h>

#include <cstring>
#include <string>

extern "C" {
#include "../../third_party/minipro/src/usb.h"
}

namespace usbdev {
namespace {

constexpr uint16_t kVid = 0xA466, kPid = 0x0A53;   // TL866II+ / T48 / T56
// One ROM (spike): running firmware with the USB plugin, its commissioned
// bootloader, and an uncommissioned RP2350's own bootloader.
bool isOneRom(uint16_t v, uint16_t p) {
  return (v == 0x1209 && (p == 0xF540 || p == 0xF542)) || (v == 0x2E8A && p == 0x000F);
}
volatile bool g_onerom = false;
uint8_t g_intf = 0;                    // the claimed interface
uint8_t g_pb_out = 0x03, g_pb_in = 0x83;   // its bulk endpoints (the bootloader's IN is 84)
constexpr uint32_t kTimeoutMs = 5000;              // usb_nix.c MP_USBTIMEOUT
constexpr uint32_t kReadTimeoutMs = 360000;        // usb_nix.c MP_USB_READ_TIMEOUT

usb_host_client_handle_t g_client = nullptr;
usb_device_handle_t g_dev = nullptr;
volatile bool g_claimed = false;
volatile uint8_t g_pending_addr = 0;
volatile bool g_gone = false;
uint16_t g_mps[16][2];                 // [ep number][0 out, 1 in]
volatile bool g_registered = false;
LogFn g_log = nullptr;
std::string g_trace;                   // every log line, for the console's 'usb'
volatile uint8_t g_other_addr = 0;     // a non-XGecu device, already described

// One transfer at a time: minipro is strictly request/response.
usb_transfer_t *g_xfer = nullptr;
size_t g_xfer_cap = 0;
SemaphoreHandle_t g_done = nullptr;

Stats g_stats[4];

int statIndex(uint8_t ep) {
  switch (ep) {
    case 0x01: return 0;
    case 0x81: return 1;
    case 0x02: return 2;
    case 0x82: return 3;
  }
  return -1;
}


void log(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_trace.size() < 16384) g_trace += std::string(buf) + "\n";
  if (g_log) g_log(buf);
}

void strDesc(const char *what, const usb_str_desc_t *sd) {
  if (!sd) return;
  char b[64];
  int n = 0;
  for (int i = 0; i < (sd->bLength - 2) / 2 && n < 63; i++) b[n++] = (char)(sd->wData[i] & 0x7F);
  b[n] = 0;
  log("  %s \"%s\"", what, b);
}

// Spike: everything about a device that is not the T48 (One ROM work).
void describe(usb_device_handle_t dev) {
  const usb_device_desc_t *dd = nullptr;
  usb_host_get_device_descriptor(dev, &dd);
  usb_device_info_t info = {};
  usb_host_device_info(dev, &info);
  log("usb: device %04x:%04x bcdUSB %04x bcdDevice %04x class %02x/%02x/%02x ep0 %u, %s speed, %u configs",
      dd->idVendor, dd->idProduct, dd->bcdUSB, dd->bcdDevice, dd->bDeviceClass, dd->bDeviceSubClass,
      dd->bDeviceProtocol, dd->bMaxPacketSize0, info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low",
      dd->bNumConfigurations);
  strDesc("manufacturer", info.str_desc_manufacturer);
  strDesc("product", info.str_desc_product);
  strDesc("serial", info.str_desc_serial_num);
  const usb_config_desc_t *cd = nullptr;
  if (usb_host_get_active_config_descriptor(dev, &cd) != ESP_OK || !cd) return;
  log("  config %u: %u interfaces, %u bytes, attr %02x, %u mA", cd->bConfigurationValue, cd->bNumInterfaces,
      cd->wTotalLength, cd->bmAttributes, cd->bMaxPower * 2);
  const uint8_t *p = (const uint8_t *)cd;
  for (int off = 0; off < cd->wTotalLength && p[off];) {
    const uint8_t len = p[off], type = p[off + 1];
    if (type == 0x04)
      log("  intf %u alt %u: %u eps, class %02x/%02x/%02x, str %u", p[off + 2], p[off + 3], p[off + 4], p[off + 5],
          p[off + 6], p[off + 7], p[off + 8]);
    else if (type == 0x05)
      log("    ep %02x attr %02x mps %u interval %u", p[off + 2], p[off + 3], p[off + 4] | (p[off + 5] << 8), p[off + 6]);
    else if (type == 0x0B)
      log("  iad: first intf %u, %u intfs, class %02x/%02x/%02x", p[off + 2], p[off + 3], p[off + 4], p[off + 5], p[off + 6]);
    else if (type != 0x02)
      log("    desc type %02x len %u", type, len);
    off += len;
  }
}

void onXfer(usb_transfer_t *) { xSemaphoreGive(g_done); }

void readEndpoints(uint8_t want) {
  const usb_config_desc_t *cd = nullptr;
  usb_host_get_active_config_descriptor(g_dev, &cd);
  memset(g_mps, 0, sizeof(g_mps));
  int off = 0;
  const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cd, want, 0, &off);
  if (!intf) return;
  for (int e = 0; e < intf->bNumEndpoints; e++) {
    int eoff = off;
    const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, e, cd->wTotalLength, &eoff);
    if (!ep) continue;
    g_mps[ep->bEndpointAddress & 0x0F][ep->bEndpointAddress & 0x80 ? 1 : 0] = ep->wMaxPacketSize & 0x7FF;
    if ((ep->bmAttributes & 3) == 2) (ep->bEndpointAddress & 0x80 ? g_pb_in : g_pb_out) = ep->bEndpointAddress;
  }
}

void openDevice(uint8_t addr) {
  if (g_dev) return;
  const esp_err_t oe = usb_host_device_open(g_client, addr, &g_dev);
  if (oe != ESP_OK) {
    log("usb: opening address %u failed (%d)", addr, (int)oe);
    g_dev = nullptr;
    return;
  }
  const usb_device_desc_t *dd = nullptr;
  usb_host_get_device_descriptor(g_dev, &dd);
  usb_device_info_t info = {};
  usb_host_device_info(g_dev, &info);
  const bool onerom = isOneRom(dd->idVendor, dd->idProduct);
  if (!onerom && (dd->idVendor != kVid || dd->idProduct != kPid)) {
    log("usb: %04x:%04x is not an XGecu programmer", dd->idVendor, dd->idProduct);
    describe(g_dev);
    g_other_addr = addr;
    usb_host_device_close(g_client, g_dev);
    g_dev = nullptr;
    return;
  }
  if (onerom) describe(g_dev);
  // picoboot is interface 1 when there are several (0 in a bare bootloader).
  const usb_config_desc_t *cd = nullptr;
  usb_host_get_active_config_descriptor(g_dev, &cd);
  g_intf = onerom && cd && cd->bNumInterfaces > 1 ? 1 : 0;
  g_onerom = onerom;
  readEndpoints(g_intf);
  if (usb_host_interface_claim(g_client, g_dev, g_intf, 0) != ESP_OK) {
    log("usb: claiming interface %u failed", g_intf);
    usb_host_device_close(g_client, g_dev);
    g_dev = nullptr;
    return;
  }
  g_claimed = true;
  log("usb: %s attached, %s speed", onerom ? "One ROM" : "programmer", info.speed == USB_SPEED_HIGH ? "high" : "full");
}

void closeDevice() {
  if (!g_dev) return;
  g_claimed = false;
  usb_host_interface_release(g_client, g_dev, g_intf);
  g_onerom = false;
  usb_host_device_close(g_client, g_dev);
  g_dev = nullptr;
  log("usb: programmer detached");
}

void onClientEvent(const usb_host_client_event_msg_t *msg, void *) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) g_pending_addr = msg->new_dev.address;
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    g_gone = true;
    g_other_addr = 0;
  }
}

// Called mid-enumeration; logs how far the handshake got.
bool onEnumFilter(const usb_device_desc_t *dd, uint8_t *config) {
  log("usb: enumerating %04x:%04x", dd->idVendor, dd->idProduct);
  *config = 1;
  return true;
}

void hostTask(void *) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

void clientTask(void *) {
  usb_host_client_config_t cfg = {};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 5;
  cfg.async.client_event_callback = onClientEvent;
  if (usb_host_client_register(&cfg, &g_client) != ESP_OK) {
    log("usb: client register failed");
    vTaskDelete(nullptr);
    return;
  }
  g_registered = true;
  uint32_t last_poll = 0;
  for (;;) {
    usb_host_client_handle_events(g_client, pdMS_TO_TICKS(20));
    // A device that enumerated before this client could hear about it gets
    // no NEW_DEV event, so look for one every second while none is open.
    if (!g_dev && millis() - last_poll > 1000) {
      last_poll = millis();
      uint8_t addrs[4];
      int n = 0;
      if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK && n > 0 && addrs[0] != g_other_addr)
        g_pending_addr = addrs[0];
    }
    if (g_gone) {
      g_gone = false;
      closeDevice();
    }
    if (g_pending_addr) {
      const uint8_t a = g_pending_addr;
      g_pending_addr = 0;
      openDevice(a);
    }
  }
}

// One bulk transfer, blocking the calling (minipro) task.
int transfer(uint8_t ep, uint8_t *buf, size_t len, int *got, uint32_t timeout_ms) {
  *got = 0;
  if (!g_claimed) return -1;
  const bool in = ep & 0x80;
  const uint16_t mps = g_mps[ep & 0x0F][in ? 1 : 0];
  if (!mps) {
    log("usb: EP %02x does not exist on this programmer", ep);
    return -1;
  }
  // The stack wants IN transfers sized to whole packets.
  const size_t n = in ? usb_round_up_to_mps(len ? len : 1, mps) : len;
  if (n > g_xfer_cap) {
    if (g_xfer) usb_host_transfer_free(g_xfer);
    g_xfer = nullptr;
    g_xfer_cap = 0;
    if (usb_host_transfer_alloc(n, 0, &g_xfer) != ESP_OK) {
      log("usb: no memory for a %u-byte transfer", (unsigned)n);
      return -1;
    }
    g_xfer_cap = n;
  }
  usb_transfer_t *t = g_xfer;
  if (!in) memcpy(t->data_buffer, buf, len);
  t->num_bytes = n;
  t->device_handle = g_dev;
  t->bEndpointAddress = ep;
  t->callback = onXfer;
  t->context = nullptr;
  t->timeout_ms = 0;
  xSemaphoreTake(g_done, 0);   // stale give from an abandoned transfer
  const int64_t t0 = esp_timer_get_time();
  if (usb_host_transfer_submit(t) != ESP_OK) {
    log("usb: submit on EP %02x failed", ep);
    return -1;
  }
  if (xSemaphoreTake(g_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    log("usb: EP %02x timed out after %u ms", ep, (unsigned)timeout_ms);
    usb_host_endpoint_halt(g_dev, ep);
    usb_host_endpoint_flush(g_dev, ep);
    xSemaphoreTake(g_done, pdMS_TO_TICKS(1000));
    usb_host_endpoint_clear(g_dev, ep);
    return -1;
  }
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
    log("usb: EP %02x transfer status %d", ep, (int)t->status);
    return -1;
  }
  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  const int si = statIndex(ep);
  if (si >= 0) {
    g_stats[si].count++;
    g_stats[si].us += us;
    if (us > g_stats[si].max_us) g_stats[si].max_us = us;
  }
  *got = t->actual_num_bytes;
  if (in) memcpy(buf, t->data_buffer, (size_t)*got < len ? (size_t)*got : len);
  return 0;
}

// usb_open() hands back a non-null cookie; the device itself lives here.
int g_cookie;

}  // namespace

void begin(LogFn log_fn) {
  g_log = log_fn;
  g_done = xSemaphoreCreateBinary();
  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  hc.root_port_unpowered = true;
  hc.enum_filter_cb = onEnumFilter;
  if (usb_host_install(&hc) != ESP_OK) {
    log("usb: host install failed");
    return;
  }
  xTaskCreatePinnedToCore(hostTask, "usbhost", 4096, nullptr, 5, nullptr, 0);
  xTaskCreatePinnedToCore(clientTask, "usbclient", 6144, nullptr, 5, nullptr, 0);
  // Power the port only once the client is listening, or a programmer that
  // is already plugged in is announced to nobody.
  for (int i = 0; i < 100 && !g_registered; i++) delay(10);
}

void powerPort(bool on) {
  const esp_err_t e = usb_host_lib_set_root_port_power(on);
  if (e != ESP_OK) log("usb: root port power %d failed (%d)", (int)on, (int)e);
}

bool attached() { return g_claimed && !g_onerom; }
bool oneRomAttached() { return g_claimed && g_onerom; }

// ---- picoboot (spike) ------------------------------------------------------
// A control transfer on EP0, blocking. data is in or out per bmRequestType.
int control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t *data, uint16_t len) {
  if (!g_dev) return -1;
  static usb_transfer_t *x = nullptr;
  if (!x && usb_host_transfer_alloc(64 + 8, 0, &x) != ESP_OK) return -1;
  auto *sp = (usb_setup_packet_t *)x->data_buffer;
  sp->bmRequestType = type;
  sp->bRequest = req;
  sp->wValue = value;
  sp->wIndex = index;
  sp->wLength = len;
  if (!(type & 0x80) && len) memcpy(x->data_buffer + 8, data, len);
  x->num_bytes = 8 + len;
  x->device_handle = g_dev;
  x->bEndpointAddress = 0;
  x->callback = onXfer;
  x->timeout_ms = 0;
  xSemaphoreTake(g_done, 0);
  if (usb_host_transfer_submit_control(g_client, x) != ESP_OK) return -2;
  if (xSemaphoreTake(g_done, pdMS_TO_TICKS(kTimeoutMs)) != pdTRUE) return -3;
  if (x->status != USB_TRANSFER_STATUS_COMPLETED) return -4;
  if ((type & 0x80) && len) memcpy(data, x->data_buffer + 8, len);
  return 0;
}

// After a stall: the command's status, then picoboot's INTERFACE_RESET and
// the halts cleared on both ends.
void picobootRecover() {
  uint8_t st[16] = {};
  if (control(0xC1, 0x42, 0, g_intf, st, 16) == 0)
    log("pb: status token %u code %u cmd %02x busy %u", st[0] | st[1] << 8, st[4], st[8], st[9]);
  control(0x41, 0x41, 0, g_intf, nullptr, 0);
  for (uint8_t ep : {g_pb_out, g_pb_in}) {
    control(0x02, 0x01, 0, ep, nullptr, 0);   // CLEAR_FEATURE(ENDPOINT_HALT)
    usb_host_endpoint_clear(g_dev, ep);
  }
}
// 32-byte command on EP 03; data on 83 (in) or 03 (out); then the other
// direction carries a zero-length acknowledgement.
int picoboot(uint8_t cmd_id, const void *args, uint8_t args_len, uint8_t *data, uint32_t len) {
  if (!oneRomAttached()) return -1;
  static uint32_t token = 1;
  uint8_t c[32] = {};
  const uint32_t magic = 0x431fd10b, tok = token++;
  memcpy(c, &magic, 4);
  memcpy(c + 4, &tok, 4);
  c[8] = cmd_id;
  c[9] = args_len;
  memcpy(c + 12, &len, 4);
  if (args_len) memcpy(c + 16, args, args_len);
  int got = 0;
  const bool in = cmd_id & 0x80;
  int rc = 0;
  uint8_t z[64];
  int data_got = 0;
  if (transfer(g_pb_out, c, 32, &got, kTimeoutMs)) rc = -2;
  else if (len && transfer(in ? g_pb_in : g_pb_out, data, len, &data_got, kTimeoutMs)) rc = -3;
  else if (transfer(in && len ? g_pb_out : g_pb_in, z, 0, &got, kTimeoutMs)) rc = -4;   // ack
  if (rc == 0 && in && len && data_got != (int)len) log("pb: short read %d/%u", data_got, (unsigned)len);
  if (rc) picobootRecover();
  return rc;
}
std::string trace() { return g_trace; }

void resetStats() { memset(g_stats, 0, sizeof(g_stats)); }
Stats stats(int i) { return g_stats[i]; }

}  // namespace usbdev

// ---- minipro's usb.h --------------------------------------------------------

extern "C" {

void *usb_open(uint8_t verbose) {
  for (int i = 0; i < 300 && !usbdev::g_claimed; i++) delay(10);
  if (!usbdev::g_claimed) {
    if (verbose) fprintf(stderr, "No programmer found.\n");
    return nullptr;
  }
  return &usbdev::g_cookie;
}

int usb_close(void *) { return 0; }   // the device stays open between runs

int minipro_get_devices_count(uint8_t version) {
  // Only the TL866II+ family (which includes the T48) is ever attached here.
  return usbdev::g_claimed && version == 5 /* MP_TL866IIPLUS */ ? 1 : 0;
}

int msg_send(void *, uint8_t *buffer, size_t size) {
  int got = 0;
  if (usbdev::transfer(0x01, buffer, size, &got, usbdev::kTimeoutMs)) return EXIT_FAILURE;
  if (got != (int)size) {
    fprintf(stderr, "IO error: expected %u bytes but %d bytes transferred\n", (unsigned)size, got);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

int msg_recv(void *, uint8_t *buffer, size_t size) {
  int got = 0;
  return usbdev::transfer(0x81, buffer, size, &got, usbdev::kReadTimeoutMs) ? EXIT_FAILURE : EXIT_SUCCESS;
}

int status_recv(void *, uint8_t *, size_t) {
  return EXIT_FAILURE;   // EP 83 is a T76 thing
}

int write_payload2(void *, uint8_t *buffer, size_t length, size_t limit) {
  if (limit && length > limit) {
    fprintf(stderr, "write_payload2: split transfers are not implemented\n");
    return EXIT_FAILURE;
  }
  int got = 0;
  if (usbdev::transfer(0x02, buffer, length, &got, usbdev::kTimeoutMs)) return EXIT_FAILURE;
  if (got != (int)length) {
    fprintf(stderr, "write_payload2: short write %d/%u\n", got, (unsigned)length);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

int read_payload2(void *, uint8_t *buffer, size_t length, size_t limit) {
  if (limit && length >= limit && length != 64) {
    fprintf(stderr, "read_payload2: split transfers are not implemented\n");
    return EXIT_FAILURE;
  }
  int got = 0;
  return usbdev::transfer(0x82, buffer, length, &got, usbdev::kTimeoutMs) ? EXIT_FAILURE : EXIT_SUCCESS;
}

}  // extern "C"
