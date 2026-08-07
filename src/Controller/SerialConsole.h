#pragma once

#include <Arduino.h>

/**
 * @brief 串口命令行控制台 — 行解析 + 命令分发
 *
 * 协议格式: <CMD> [key=value ...]\r\n
 * 回应:     OK <cmd> [...] 或 ERR <cmd> code=<n>
 *
 * 支持命令: HELP, STATUS, CONFIG, SET, TX, SWEEP, STOP, WIFI, SCAN, SAVE, RESET, PING
 *
 * 使用方式:
 * @code
 *   SerialConsole console(Serial);
 *   console.begin();
 *   // 在 loop() 中:
 *   console.loop();
 * @endcode
 */
class SerialConsole {
public:
  /**
   * @param stream 串口流 (通常是 Serial)
   */
  SerialConsole(Stream& stream);

  /** 打印启动横幅 (芯片信息, WiFi 状态, IP 地址) */
  void begin();

  /** 每帧轮询, 处理输入行 */
  void loop();

  /** 写入一行响应 (供外部回调使用) */
  void writeResponse(const char* line);

  /** printf 风格输出 (转发到 m_stream) */
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

private:
  Stream& m_stream;
  char    m_lineBuf[384];
  int     m_linePos;

  void processLine(const char* line);
  void printHelp();
  void printBanner();

  // 命令处理
  void cmdHelp();
  void cmdStatus();
  void cmdConfig();
  void cmdSet(const char* args);
  void cmdTx(const char* args);
  void cmdWifi(const char* args);
  void cmdScan();
  void cmdSweep(const char* args);
  void cmdSweepStatus();
  void cmdStop();
  void cmdReset();
  void cmdPing();
  void cmdSave();
};
