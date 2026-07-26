#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/**
 * @brief Web 服务器 — REST API + LittleFS 静态文件
 *
 * 监听 80 端口, 提供:
 *   - 静态文件 (index.html, css/, js/) from LittleFS
 *   - REST API 端点 (JSON 请求/响应)
 *   - API 响应委托给 api_dispatch() 统一处理
 *
 * 使用方式:
 * @code
 *   WebServer web(80);
 *   web.begin();
 *   // AsyncWebServer 在后台处理, 无需 loop()
 * @endcode
 */
class WebServer {
public:
  explicit WebServer(uint16_t port = 80);
  ~WebServer();

  /**
   * @brief 启动服务器, 挂载 LittleFS 并注册路由。
   * @return true on success, false if LittleFS mount fails
   */
  bool begin();

  /** 服务器端口 */
  uint16_t port() const { return m_port; }

private:
  AsyncWebServer m_server;
  uint16_t       m_port;

  // ---- 路由注册 ----
  void setupRoutes();
  void setupStaticFiles();

  // ---- JSON 辅助 ----
  static String configToJson(const struct TxConfig& cfg);
  static void   jsonToConfig(const char* json, struct TxConfig& cfg);
  static String statusToJson(const struct DeviceStatus& s);
  static String wifiToJson(const struct WiFiStatus& w);
  static String errorJson(int code, const char* detail);

  // ---- API 处理器 ----
  void onPing(AsyncWebServerRequest* req);
  void onStatus(AsyncWebServerRequest* req);
  void onConfigGet(AsyncWebServerRequest* req);
  void onConfigSet(AsyncWebServerRequest* req, uint8_t* data, size_t len);
  void onTransmit(AsyncWebServerRequest* req, uint8_t* data, size_t len);
  void onReset(AsyncWebServerRequest* req);
  void onWifiStatus(AsyncWebServerRequest* req);
  void onWifiStatusQuick(AsyncWebServerRequest* req);
  void onWifiSet(AsyncWebServerRequest* req, uint8_t* data, size_t len);
  void onWifiScan(AsyncWebServerRequest* req);
  void onGetLog(AsyncWebServerRequest* req);
};
