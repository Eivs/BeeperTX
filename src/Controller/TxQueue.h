#pragma once

#include <WString.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 异步请求队列 — 承载由主循环 (api_loop) 执行的延迟动作。
 *
 * 从 API.cpp 抽出, 把 "待发射 / 待扫描 WiFi / 待设置 WiFi 凭据" 三类
 * 状态从命令调度器中剥离。命令处理器只设置标记, 实际执行 (可能阻塞
 * 数百 ms 的发射、WiFi 扫描/连接) 统一在 loop() 中进行, 避免卡住
 * AsyncWebServer 回调上下文。
 *
 * 所有访问均发生在 loopTask (Arduino 主循环) 单线程上下文:
 * Web/串口回调仅通过 setXxx/triggerXxx 写入, loop() 消费。无需加锁。
 */
class TxQueue {
public:
  // ---- 待发射 ----

  /** 标记一次待发射; 返回 false 表示已有待发射 (调用方应报 BUSY) */
  bool setPendingTx(const char* msgOverride);

  /** 消费待发射请求; 返回 true 且有待发, 消息拷入 msgOut */
  bool consumePendingTx(char* msgOut, size_t size);

  bool isPendingTx() const { return m_pendingTx; }

  // ---- WiFi 后台扫描 ----

  /** 触发后台扫描 (由 api_loop 执行) */
  void triggerScan() { m_pendingScan = true; }

  /** 消费扫描请求; 返回是否应执行扫描 */
  bool consumeScan() {
    bool p = m_pendingScan;
    m_pendingScan = false;
    return p;
  }

  /** 保存扫描结果缓存 (JSON 数组字符串) */
  void setScanCache(const char* json) { m_scanCache = json ? json : "[]"; }

  /** 扫描结果缓存 */
  const char* scanCache() const { return m_scanCache.c_str(); }

  // ---- 待设置 WiFi 凭据 ----

  /** 排队一组凭据; 返回 false 表示已有待处理凭据 */
  bool setPendingWifi(const char* ssid, const char* pass);

  /** 消费凭据; 返回 true 且有待处理 */
  bool consumeWifi(char* ssidOut, size_t ssidSize,
                   char* passOut, size_t passSize);

  /** 复位全部状态 */
  void clear();

private:
  bool   m_pendingTx = false;
  char   m_pendingTxMsg[256] = {0};

  bool   m_pendingScan = false;
  String m_scanCache = "[]";

  bool   m_pendingWifiSet = false;
  char   m_pendingWifiSsid[33] = {0};
  char   m_pendingWifiPass[65] = {0};
};
