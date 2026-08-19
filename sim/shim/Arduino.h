// Minimal Arduino core shim, just enough for Adafruit_GFX and the shared
// dashboard layout code to compile on a host. Not a general Arduino emulation:
// only the surface those two actually touch is provided.

#ifndef SIM_ARDUINO_H
#define SIM_ARDUINO_H

// ARDUINO itself is defined by the build (see sim/Makefile): Adafruit_GFX.h
// tests it before it includes this header.
#ifndef ARDUINO
#define ARDUINO 10819
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

// Fonts and bitmaps are plain const arrays on a host.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char*
#endif

// Adafruit_GFX.cpp defines its own pgm_read_* fallbacks when these are absent,
// so deliberately do not define them here.

#if defined(SIM_PROVIDE_STRLCPY)
inline size_t strlcpy(char* destination, const char* source, size_t size) {
  const size_t length = strlen(source);
  if (size != 0) {
    const size_t copied = length < size - 1 ? length : size - 1;
    memcpy(destination, source, copied);
    destination[copied] = '\0';
  }
  return length;
}
#endif

inline void yield() {}
inline void delay(unsigned long) {}

// Arduino math helpers used by Adafruit_GFX. min/max are deliberately omitted:
// Adafruit_GFX.cpp defines its own and a duplicate here would conflict.
#ifndef DEG_TO_RAD
#define DEG_TO_RAD 0.017453292519943295769236907684886
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG 57.295779513082320876798154814105
#endif
#ifndef radians
#define radians(deg) ((deg) * DEG_TO_RAD)
#endif
#ifndef degrees
#define degrees(rad) ((rad) * RAD_TO_DEG)
#endif
#ifndef sq
#define sq(x) ((x) * (x))
#endif
#ifndef constrain
#define constrain(amount, low, high) \
  ((amount) < (low) ? (low) : ((amount) > (high) ? (high) : (amount)))
#endif

class __FlashStringHelper;
#define F(string_literal) (reinterpret_cast<const __FlashStringHelper*>(string_literal))

// Adafruit_GFX declares a getTextBounds(const String&) overload, so the type has
// to exist. The dashboard layout only ever passes const char*.
class String {
 public:
  String() = default;
  String(const char* text) : value_(text ? text : "") {}
  String(const std::string& text) : value_(text) {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  char charAt(size_t index) const { return value_[index]; }
  char operator[](size_t index) const { return value_[index]; }

  String& operator+=(const String& other) {
    value_ += other.value_;
    return *this;
  }

 private:
  std::string value_;
};

#include "Print.h"

#endif  // SIM_ARDUINO_H
