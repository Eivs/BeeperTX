// 宿主机测试 stub — Arduino 框架最小替代
// ⚠ 仅测试用精简接口: 勿当作生产代码修改; 仅覆盖宿主机编译所需子集
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

// portMUX stub (生产代码为 ESP32 临界区原语)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static inline void taskENTER_CRITICAL(portMUX_TYPE*) {}
static inline void taskEXIT_CRITICAL(portMUX_TYPE*) {}

class FakeSerialClass {
public:
  void print(const char* s) { (void)s; }
  void print(char c) { (void)c; }
  void print(unsigned long v) { (void)v; }
  void println(const char* s) { (void)s; }
  void println() {}
};
extern FakeSerialClass Serial;
