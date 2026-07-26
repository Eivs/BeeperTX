#include "WiFiManager.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>

const char* WiFiManager::NVS_NS = "wifi";

WiFiManager::WiFiManager()
  : m_state(DISCONNECTED)
  , m_lastCheck(0)
{
  memset(m_ip, 0, sizeof(m_ip));
  memset(m_ssid, 0, sizeof(m_ssid));
}

WiFiManager::~WiFiManager() {}

// ============================================================
// 启动
// ============================================================

void WiFiManager::begin() {
  char ssid[33] = {0};
  char pass[65] = {0};

  if (loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("BeeperTX");
    WiFi.begin(ssid, pass);

    m_state = CONNECTING;
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
      m_state = CONNECTED;
      strncpy(m_ssid, ssid, sizeof(m_ssid) - 1);
      strncpy(m_ip, WiFi.localIP().toString().c_str(), sizeof(m_ip) - 1);
      return;
    }
  }

  // 无凭据或连接失败 → AP 模式
  startAP();
}

// ============================================================
// AP 模式
// ============================================================

void WiFiManager::startAP() {
  WiFi.mode(WIFI_AP_STA);   // 双模式: AP 保持连接 + STA 用于扫描
  WiFi.softAP("BeeperTX");
  m_state = AP_MODE;
  strncpy(m_ssid, "BeeperTX", sizeof(m_ssid) - 1);
  strncpy(m_ip, WiFi.softAPIP().toString().c_str(), sizeof(m_ip) - 1);

  // 启动 DNS 劫持 — 所有域名都解析到 ESP32 自身 IP, 实现 Captive Portal
  m_dns.start(53, "*", WiFi.softAPIP());
}

// ============================================================
// Loop
// ============================================================

void WiFiManager::loop() {
  // DNS 劫持处理 (AP 模式下实时响应)
  if (m_state == AP_MODE) {
    m_dns.processNextRequest();
  }

  // 每 10 秒检查一次连接状态
  if (millis() - m_lastCheck < 10000) return;
  m_lastCheck = millis();

  if (m_state == CONNECTED) {
    if (WiFi.status() != WL_CONNECTED) {
      // 断线 → 尝试重连
      char ssid[33] = {0};
      char pass[65] = {0};
      if (loadCredentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        WiFi.reconnect();
        m_state = CONNECTING;
        m_connectStart = millis();
      }
    }
  } else if (m_state == CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      m_state = CONNECTED;
      strncpy(m_ip, WiFi.localIP().toString().c_str(), sizeof(m_ip) - 1);
    } else if (millis() - m_connectStart > 10000) {
      // 超时 → 回退到 AP 模式
      startAP();
    }
  }
}

// ============================================================
// NVS 凭据
// ============================================================

bool WiFiManager::loadCredentials(char* ssid, size_t ssidLen,
                                   char* pass, size_t passLen) {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, true)) return false;
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  prefs.end();

  if (s.length() == 0) return false;
  strncpy(ssid, s.c_str(), ssidLen - 1);
  strncpy(pass, p.c_str(), passLen - 1);
  return true;
}

void WiFiManager::saveCredentials(const char* ssid, const char* pass) {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.putString("ssid", String(ssid));
  prefs.putString("pass", String(pass));
  prefs.end();
}

void WiFiManager::setCredentials(const char* ssid, const char* password) {
  saveCredentials(ssid, password);
  // 尝试连接新网络
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("BeeperTX");
  WiFi.begin(ssid, password);
  strncpy(m_ssid, ssid, sizeof(m_ssid) - 1);
  m_state = CONNECTING;
}

void WiFiManager::clearCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
}

// ============================================================
// WiFi 扫描
// ============================================================

String WiFiManager::scanNetworks() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"";
    json += WiFi.SSID(i);
    json += "\",\"rssi\":";
    json += WiFi.RSSI(i);
    json += ",\"secure\":";
    json += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false");
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
  return json;
}
