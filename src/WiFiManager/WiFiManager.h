#pragma once

#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>

/**
 * @brief WiFi 管理器 — AP 配网 / STA 自动连接 / NVS 凭据持久化
 *
 * 启动流程:
 *   1. 检查 NVS 中是否有 WiFi 凭据
 *   2. 有凭据 → STA 模式连接 (10s 超时)
 *      - 成功 → hostname="BeeperTX", 返回 IP
 *      - 失败 → 回退到 AP 模式
 *   3. 无凭据 → AP 模式
 *      - SSID="BeeperTX", 开放网络, IP=192.168.4.1
 *
 * 使用方式:
 * @code
 *   WiFiManager wifi;
 *   wifi.begin();
 *   if (wifi.isConnected()) {
 *     Serial.println(wifi.ipAddress());
 *   }
 * @endcode
 */
class WiFiManager {
public:
  enum State {
    DISCONNECTED = 0,
    CONNECTING   = 1,
    CONNECTED    = 2,
    AP_MODE      = 3
  };

  WiFiManager();
  ~WiFiManager();

  /**
   * @brief 启动 WiFi (AP 或 STA, 根据 NVS 凭据自动决定)。
   *        阻塞至连接成功或超时回退到 AP。
   */
  void begin();

  /**
   * @brief 主循环更新 — 监控连接状态, 断线自动重连/回退 AP。
   *        每帧在 main loop() 中调用。
   */
  void loop();

  /** 当前状态 */
  State state() const { return m_state; }

  /** STA 连接成功时返回 true */
  bool isConnected() const { return m_state == CONNECTED; }

  /** 当前 IP 地址字符串 */
  const char* ipAddress() const { return m_ip; }

  /** 当前 WiFi SSID (STA 模式下为路由 SSID, AP 模式下为 "BeeperTX") */
  const char* ssid() const { return m_ssid; }

  /** DNS 服务器 (仅 AP 模式有效), 需在 loop() 中调用 processNextRequest() */
  DNSServer& dnsServer() { return m_dns; }

  // ---- 凭据管理 ----

  /**
   * @brief 保存 WiFi 凭据到 NVS 并立即尝试连接 (无需重启)。
   *        连接结果由 loop() 维护: 成功 → STA 模式; 10s 超时 → 回退 AP。
   */
  void setCredentials(const char* ssid, const char* password);

  /** 清除 NVS 中保存的凭据 */
  void clearCredentials();

  /**
   * @brief 扫描附近 WiFi 网络, 返回 JSON 数组字符串。
   *        格式: [{"ssid":"MyWiFi","rssi":-45,"secure":true}, ...]
   */
  String scanNetworks();

private:
  State  m_state;
  char   m_ip[16];
  char   m_ssid[33];
  unsigned long m_lastCheck;
  unsigned long m_connectStart;
  DNSServer m_dns;

  static const char* NVS_NS;

  void startAP();
  bool connectSTA();
  bool loadCredentials(char* ssid, size_t ssidLen, char* pass, size_t passLen);
  void saveCredentials(const char* ssid, const char* pass);
};
