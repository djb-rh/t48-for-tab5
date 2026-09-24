// The T48 on the Tab5's USB-A port, behind minipro's usb.h (see usb_esp.cpp).
#pragma once

#include <cstdint>

namespace usbdev {

using LogFn = void (*)(const char *line);

// Installs the host stack and starts its tasks. The port is left unpowered;
// call powerPort(true) once the 5 V rail is on.
void begin(LogFn log);
void powerPort(bool on);
// A programmer is enumerated and its interface claimed.
bool attached();

// Transfer timing since the last reset, for finding where a job's time goes.
struct Stats {
  uint32_t count = 0;
  uint64_t us = 0;        // submit to completion, summed
  uint32_t max_us = 0;
};
void resetStats();
Stats stats(int ep_index);   // 0: EP01 out, 1: EP81 in, 2: EP02 out, 3: EP82 in

}  // namespace usbdev
