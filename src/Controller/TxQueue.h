#pragma once

#include <Arduino.h>   // portMUX_TYPE (与 LogRingBuffer 同款临界区)
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
 * 并发模型 (2026-08 修订): 单消费者 (api_loop, 由 main.cpp loopMainTask
 * 在 core 0 prio 1 执行), 但生产者为两个:
 *   - Web 回调 (AsyncTCP 服务任务, core 0 prio 3, 由 platformio.ini 固定);
 *   - 串口回调 (SerialConsole::loop → api_dispatch, 与消费者同任务)。
 * 两个生产者可能在同一任务切换窗口内先后执行 setXxx, 若 check-then-act
 * 无保护, 后到者会静默覆盖先到者的消息 (web 已回 OK 但消息丢失)。
 * 因此 pending 标志 + 数据的检查/置位/写入整体由 portMUX 临界区保护
 * (操作均为短 strncpy, 无阻塞调用, 临界区开销可忽略)。
 * 扫描结果缓存 (m_scanCache) 同理, 由独立 m_scanMux 临界区保护。
 */
class TxQueue {
public:
  // ---- 待发射 ----

  /** 标记一次待发射; 返回 false 表示已有待发射 (调用方应报 BUSY) */
  bool setPendingTx(const char* msgOverride);

  /** 消费待发射请求; 返回 true 且有待发, 消息拷入 msgOut */
  bool consumePendingTx(char* msgOut, size_t size);

  /** 预检是否有待发射 (无锁 bool 读, 竞态容忍: 最终以 setPendingTx 为准) */
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

  /** 保存扫描结果缓存 (JSON 数组字符串) — 临界区写入 */
  void setScanCache(const char* json);

  /**
   * @brief 拷贝扫描结果缓存到调用方缓冲 (临界区读取)。
   * 固定 char 缓冲而非 String: String 重分配会释放旧指针, 其他任务
   * (web 回调) 持指针读取即 use-after-free。
   */
  void getScanCache(char* out, size_t size);

  /** 扫描结果缓存最大长度 (与旧 union 4KB 一致) */
  static const size_t SCAN_CACHE_MAX = 4096;

  // ---- 待设置 WiFi 凭据 ----

  /** 排队一组凭据; 返回 false 表示已有待处理凭据 */
  bool setPendingWifi(const char* ssid, const char* pass);

  /** 消费凭据; 返回 true 且有待处理 */
  bool consumeWifi(char* ssidOut, size_t ssidSize,
                   char* passOut, size_t passSize);

  /** 复位全部状态 */
  void clear();

private:
  // pending 标志 + 数据共享于双生产者 (web/串口回调) 与消费者 (api_loop)
  // 之间, 由 m_pendingMux 临界区保护 check-then-act 与数据写入。
  portMUX_TYPE m_pendingMux = portMUX_INITIALIZER_UNLOCKED;

  bool   m_pendingTx = false;
  char   m_pendingTxMsg[256] = {0};

  bool   m_pendingScan = false;
  char   m_scanCache[SCAN_CACHE_MAX] = "[]";   // 固定缓冲 (临界区保护)
  portMUX_TYPE m_scanMux = portMUX_INITIALIZER_UNLOCKED;

  bool   m_pendingWifiSet = false;
  char   m_pendingWifiSsid[33] = {0};
  char   m_pendingWifiPass[65] = {0};
};
