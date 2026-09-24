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

extern "C" {
#include "../../third_party/minipro/src/usb.h"
}

namespace usbdev {
namespace {

constexpr uint16_t kVid = 0xA466, kPid = 0x0A53;   // TL866II+ / T48 / T56
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
  if (g_log) g_log(buf);
}

void onXfer(usb_transfer_t *) { xSemaphoreGive(g_done); }

void readEndpoints() {
  const usb_config_desc_t *cd = nullptr;
  usb_host_get_active_config_descriptor(g_dev, &cd);
  memset(g_mps, 0, sizeof(g_mps));
  int off = 0;
  const usb_intf_desc_t *intf = usb_parse_interface_descriptor(cd, 0, 0, &off);
  if (!intf) return;
  for (int e = 0; e < intf->bNumEndpoints; e++) {
    int eoff = off;
    const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, e, cd->wTotalLength, &eoff);
    if (ep) g_mps[ep->bEndpointAddress & 0x0F][ep->bEndpointAddress & 0x80 ? 1 : 0] = ep->wMaxPacketSize & 0x7FF;
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
  if (dd->idVendor != kVid || dd->idProduct != kPid) {
    log("usb: %04x:%04x is not an XGecu programmer", dd->idVendor, dd->idProduct);
    usb_host_device_close(g_client, g_dev);
    g_dev = nullptr;
    return;
  }
  readEndpoints();
  if (usb_host_interface_claim(g_client, g_dev, 0, 0) != ESP_OK) {
    log("usb: claiming interface 0 failed");
    usb_host_device_close(g_client, g_dev);
    g_dev = nullptr;
    return;
  }
  g_claimed = true;
  log("usb: programmer attached, %s speed", info.speed == USB_SPEED_HIGH ? "high" : "FULL");
}

void closeDevice() {
  if (!g_dev) return;
  g_claimed = false;
  usb_host_interface_release(g_client, g_dev, 0);
  usb_host_device_close(g_client, g_dev);
  g_dev = nullptr;
  log("usb: programmer detached");
}

void onClientEvent(const usb_host_client_event_msg_t *msg, void *) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) g_pending_addr = msg->new_dev.address;
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) g_gone = true;
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
      if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK && n > 0) g_pending_addr = addrs[0];
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

bool attached() { return g_claimed; }

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
