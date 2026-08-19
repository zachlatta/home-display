// Arduino Print shim. Adafruit_GFX derives from Print and implements
// write(uint8_t); these overloads route the dashboard's print() calls into it.

#ifndef SIM_PRINT_H
#define SIM_PRINT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

class String;

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t character) = 0;

  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t written = 0;
    while (size--) written += write(*buffer++);
    return written;
  }

  size_t print(const char* text) {
    if (!text) return 0;
    return write(reinterpret_cast<const uint8_t*>(text), strlen(text));
  }

  size_t print(char character) { return write(static_cast<uint8_t>(character)); }

  size_t print(int value) { return printNumber("%d", value); }
  size_t print(unsigned int value) { return printNumber("%u", value); }
  size_t print(long value) { return printNumber("%ld", value); }
  size_t print(unsigned long value) { return printNumber("%lu", value); }
  size_t print(double value) { return printNumber("%g", value); }

  size_t println() { return print("\r\n"); }

 private:
  template <class T>
  size_t printNumber(const char* format, T value) {
    char buffer[32];
    const int length = snprintf(buffer, sizeof(buffer), format, value);
    if (length <= 0) return 0;
    return write(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(length));
  }
};

#endif  // SIM_PRINT_H
