// stub — Arduino Stream/Print 最小实现
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
typedef uint8_t byte;
typedef uint16_t word;
#define F(x) (x)
#define PROGMEM
#define PSTR(x) (x)
extern unsigned long g_millis;
extern unsigned long millis();
static inline void delay(unsigned long) {}
enum _PrintEnum { HEX = 16 };
class ESPClass { public: void restart() {} };
extern ESPClass ESP;
class Print {
public:
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* b, size_t n) = 0;
  size_t write(const char* s) { return write((const uint8_t*)s, strlen(s)); }
  void print(const char* s) { write((const uint8_t*)s, strlen(s)); }
  void print(char c) { write((uint8_t)c); }
  void print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); print(b); }
  void print(unsigned int v) { char b[16]; snprintf(b, sizeof(b), "%u", v); print(b); }
  void print(long v) { char b[16]; snprintf(b, sizeof(b), "%ld", v); print(b); }
  void print(unsigned long v) { char b[16]; snprintf(b, sizeof(b), "%lu", v); print(b); }
  void print(double v, int p = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", p, v); print(b); }
  void println() { write((uint8_t)'\n'); }
  void println(const char* s) { print(s); println(); }
  void println(char c) { print(c); println(); }
  void println(int v) { print(v); println(); }
  void println(unsigned int v) { print(v); println(); }
  void println(long v) { print(v); println(); }
  void println(unsigned long v) { print(v); println(); }
  void println(double v, int p = 2) { print(v, p); println(); }
};
class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
};
extern unsigned long millis();
