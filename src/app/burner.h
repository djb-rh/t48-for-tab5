// The burner's start-up and main loop, for main.cpp or a launcher (host.h).
#pragma once

namespace burner {

void setup();
void loop();
// The backlight, off or back on (host.h: blank_ms does this by itself).
void blank(bool on);
bool blanked();

}  // namespace burner
