// The T48 on the Tab5's USB-A port, behind minipro's usb.h (see usb_esp.cpp).
#pragma once

namespace usbdev {

using LogFn = void (*)(const char *line);

// Installs the host stack and starts its tasks. The port is left unpowered;
// call powerPort(true) once the 5 V rail is on.
void begin(LogFn log);
void powerPort(bool on);
// A programmer is enumerated and its interface claimed.
bool attached();

}  // namespace usbdev
