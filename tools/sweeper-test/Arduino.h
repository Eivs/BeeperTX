// 宿主机测试 stub — Arduino 框架最小替代
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t byte;
typedef uint16_t word;
#define F(x) (x)
#define PROGMEM
#define PSTR(x) (x)

extern unsigned long g_millis;
static inline unsigned long millis() { return g_millis; }
static inline void delay(unsigned long) {}

class FakeSerialClass {
public:
  void print(const char* s) { (void)s; }
  void print(char c) { (void)c; }
  void print(unsigned long v) { (void)v; }
  void println(const char* s) { (void)s; }
  void println() {}
};
extern FakeSerialClass Serial;
