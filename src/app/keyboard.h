// The Tab5 Keyboard, M5Stack's 70-key clip-on for the 2x5 header.
//
// It is an STM32 on its own I2C bus (SDA G0, SCL G1, INT G50, address 0x6D)
// that queues key events and reports them in one of three shapes. This uses
// its HID shape - a modifier byte and a USB usage code - and turns that into
// characters and a few named keys here, on the US layout it carries.
//
// Presence is not configured: the bus is probed a few times a second while
// nothing answers, so clipping the keyboard on mid-session is noticed, and
// once found it is read whenever its interrupt line says there is something.
#pragma once

#include <cstdint>

namespace burner {
namespace keyboard {

enum class Special : uint8_t {
  None, Enter, Backspace, Escape, Tab, Up, Down, Left, Right, Delete,
  PageUp, PageDown, Home, End,
};

struct Key {
  char ch = 0;               // the character, or 0 for a named key
  Special special = Special::None;
  bool ctrl = false, alt = false;
};

void begin();
void tick(uint32_t now_ms);
bool present();
// The next key typed, if any. Keys are queued, so a fast typist loses none.
bool take(Key *out);
// Queues a key as if it had been typed (serial test console).
void inject(const Key &k);
// Prints what the keyboard has sent since boot, for bring-up over serial.
void dumpLog();

}  // namespace keyboard
}  // namespace burner
