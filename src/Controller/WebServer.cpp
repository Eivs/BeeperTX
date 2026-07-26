#include "WebServer.h"
#include "API.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

WebServer::WebServer(uint16_t port)
  : m_server(port)
  , m_port(port)
{}

WebServer::~WebServer() {}

// ============================================================
// 启动
// ============================================================

bool WebServer::begin() {
  if (!LittleFS.begin(true)) {
    Serial.println(F("WebServer: LittleFS mount FAILED"));
    return false;
  }

  setupRoutes();
  setupStaticFiles();
  m_server.begin();
  Serial.printf("WebServer: listening on port %d\n", m_port);
  return true;
}

// ============================================================
// 静态文件
// ============================================================

void WebServer::setupStaticFiles() {
  m_server.serveStatic("/", LittleFS, "/")
    .setDefaultFile("index.html");
}

// ============================================================
// 路由注册
// ============================================================

void WebServer::setupRoutes() {
  // ---- 系统 ----
  m_server.on("/api/ping", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onPing(req);
  });

  // ---- 状态 ----
  m_server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onStatus(req);
  });

  // ---- 配置 ----
  m_server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onConfigGet(req);
  });
  m_server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest*) {},
    nullptr,
    [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      onConfigSet(req, data, len);
    });

  // ---- 发射 ----
  m_server.on("/api/transmit", HTTP_POST,
    [](AsyncWebServerRequest*) {},
    nullptr,
    [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      onTransmit(req, data, len);
    });

  // ---- 重启 ----
  m_server.on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
    onReset(req);
  });

  // ---- WiFi ----
  m_server.on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onWifiStatusQuick(req);
  });
  m_server.on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onWifiScan(req);
  });
  m_server.on("/api/wifi", HTTP_POST,
    [](AsyncWebServerRequest*) {},
    nullptr,
    [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      onWifiSet(req, data, len);
    });

  // ---- 日志 ----
  m_server.on("/api/log", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onGetLog(req);
  });

  // ---- CORS (开发调试用) ----
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  // ---- Captive Portal 检测端点 (各操作系统探测 URL) ----
  // Android
  m_server.on("/generate_204", HTTP_GET, [this](AsyncWebServerRequest* req) {
    req->redirect("/");
  });
  // Apple iOS/macOS
  m_server.on("/hotspot-detect.html", HTTP_GET, [this](AsyncWebServerRequest* req) {
    req->redirect("/");
  });
  // Windows
  m_server.on("/ncsi.txt", HTTP_GET, [this](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "Microsoft NCSI");
  });
  m_server.on("/connecttest.txt", HTTP_GET, [this](AsyncWebServerRequest* req) {
    req->redirect("/");
  });
  // Firefox
  m_server.on("/success.txt", HTTP_GET, [this](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "success");
  });
  // 通用 fallback: 非 API 路径重定向到首页 (避免自引用死循环)
  m_server.onNotFound([](AsyncWebServerRequest* req) {
    if (req->url().startsWith("/api/")) {
      req->send(404, "application/json", "{\"code\":1,\"detail\":\"not found\"}");
    } else if (req->url() != "/") {
      req->redirect("/");
    } else {
      req->send(503, "text/html",
        "<h2>BeeperTX</h2><p>Web files missing from LittleFS.</p>"
        "<p>Run: <code>pio run --target uploadfs</code></p>");
    }
  });
}

// ============================================================
// JSON 序列化
// ============================================================

String WebServer::configToJson(const TxConfig& cfg) {
  JsonDocument doc;
  doc["encoding"]  = (cfg.encoding == EncodingType::POCSAG) ? "pocsag" : "flex";
  doc["phase"]     = (cfg.phase == PhaseType::NORMAL) ? "normal" : "inverted";
  doc["tone"]      = String((char)('A' + (int)cfg.tone));
  doc["msgType"]   = (cfg.msgType == MsgType::NUMERIC) ? "numeric" : "alphanumeric";
  doc["freqKHz"]   = cfg.freqKHz;
  doc["baudRate"]  = cfg.baudRate;
  doc["address"]   = cfg.address;
  doc["txPower"]   = cfg.txPower;
  doc["message"]   = String(cfg.message);

  String out;
  serializeJson(doc, out);
  return out;
}

void WebServer::jsonToConfig(const char* json, TxConfig& cfg) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return;

  if (doc["encoding"].is<const char*>()) {
    const char* v = doc["encoding"];
    cfg.encoding = (strcmp(v, "flex") == 0) ? EncodingType::FLEX : EncodingType::POCSAG;
  }
  if (doc["phase"].is<const char*>()) {
    const char* v = doc["phase"];
    cfg.phase = (strcmp(v, "inverted") == 0) ? PhaseType::INVERTED : PhaseType::NORMAL;
  }
  if (doc["tone"].is<const char*>()) {
    const char* v = doc["tone"];
    if (*v >= 'a' && *v <= 'd') cfg.tone = static_cast<ToneType>(*v - 'a');
    else if (*v >= 'A' && *v <= 'D') cfg.tone = static_cast<ToneType>(*v - 'A');
  }
  if (doc["msgType"].is<const char*>()) {
    const char* v = doc["msgType"];
    cfg.msgType = (strcmp(v, "numeric") == 0) ? MsgType::NUMERIC : MsgType::ALPHANUMERIC;
  }
  if (doc["freqKHz"].is<uint32_t>() || doc["freqKHz"].is<const char*>())
    cfg.freqKHz    = doc["freqKHz"].as<uint32_t>();
  if (doc["baudRate"].is<uint16_t>() || doc["baudRate"].is<const char*>())
    cfg.baudRate   = doc["baudRate"].as<uint16_t>();
  if (doc["address"].is<uint32_t>() || doc["address"].is<const char*>())
    cfg.address    = doc["address"].as<uint32_t>();
  if (doc["txPower"].is<uint8_t>() || doc["txPower"].is<const char*>())
    cfg.txPower    = doc["txPower"].as<uint8_t>();
  if (doc["message"].is<const char*>()) {
    String msg = doc["message"].as<String>();
    strncpy(cfg.message, msg.c_str(), sizeof(cfg.message) - 1);
    cfg.message[sizeof(cfg.message) - 1] = '\0';
  }
}

String WebServer::statusToJson(const DeviceStatus& s) {
  JsonDocument doc;
  doc["partId"]    = s.partId;
  doc["romId"]     = s.romId;
  doc["chipRev"]   = s.chipRev;
  doc["state"]     = s.state;
  doc["freqKHz"]   = s.freqKHz;
  doc["txPower"]   = s.txPower;
  doc["rssiDbm"]   = s.rssiDbm;
  doc["uptimeS"]   = s.uptimeS;
  doc["txCount"]   = s.txCount;

  String out;
  serializeJson(doc, out);
  return out;
}

String WebServer::wifiToJson(const WiFiStatus& w) {
  JsonDocument doc;
  doc["state"] = w.state;
  doc["ssid"]  = String(w.ssid);
  doc["ip"]    = String(w.ip);
  doc["rssi"]  = w.rssi;

  String out;
  serializeJson(doc, out);
  return out;
}

String WebServer::errorJson(int code, const char* detail) {
  JsonDocument doc;
  doc["code"]   = code;
  doc["detail"] = String(detail ? detail : "");

  String out;
  serializeJson(doc, out);
  return out;
}

// ============================================================
// API 处理器
// ============================================================

void WebServer::onPing(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_PING;
  api_dispatch(&r, &resp);

  JsonDocument doc;
  doc["uptime"] = resp.detail;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

void WebServer::onStatus(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_GET_STATUS;
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    req->send(200, "application/json", statusToJson(resp.status));
  } else {
    req->send(500, "application/json", errorJson((int)resp.code, "status failed"));
  }
}

void WebServer::onConfigGet(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_GET_CONFIG;
  api_dispatch(&r, &resp);

  req->send(200, "application/json", configToJson(resp.config));
}

void WebServer::onConfigSet(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
  // 先获取当前配置, 再 patch
  CmdRequest getR; CmdResponse getResp;
  memset(&getR, 0, sizeof(getR));
  getR.cmd = CmdID::CMD_GET_CONFIG;
  api_dispatch(&getR, &getResp);
  TxConfig cfg = getResp.config;

  // 用 JSON body 覆盖
  String body;
  body.concat((char*)data, len);
  jsonToConfig(body.c_str(), cfg);

  // 提交
  CmdRequest setR; CmdResponse setResp;
  memset(&setR, 0, sizeof(setR));
  setR.cmd = CmdID::CMD_SET_CONFIG;
  setR.setConfig = cfg;
  api_dispatch(&setR, &setResp);

  if (setResp.code == RespCode::OK) {
    char logLine[128];
    snprintf(logLine, sizeof(logLine), "WEB config: freq=%ukHz baud=%d pwr=%d",
             (unsigned)cfg.freqKHz, cfg.baudRate, cfg.txPower);
    api_log(logLine);
    req->send(200, "application/json", configToJson(cfg));
  } else {
    char errMsg[64];
    snprintf(errMsg, sizeof(errMsg), "code=%d detail=%d",
             (int)setResp.code, (int)setResp.detail);
    api_log(errMsg);
    req->send(400, "application/json", errorJson((int)setResp.code, errMsg));
  }
}

void WebServer::onTransmit(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_TRANSMIT;

  // 可选消息覆盖
  if (data && len > 0) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, (char*)data, len);
    if (!err && doc["message"].is<const char*>()) {
      String msg = doc["message"].as<String>();
      strncpy(r.transmitMsg, msg.c_str(), sizeof(r.transmitMsg) - 1);
    }
  }

  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    api_log("WEB TX sent");
    req->send(200, "application/json", "{\"code\":0,\"detail\":\"ok\"}");
  } else {
    api_log("WEB TX: FAILED");
    req->send(400, "application/json", errorJson((int)resp.code, "tx failed"));
  }
}

void WebServer::onReset(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_RESET;
  api_dispatch(&r, &resp);

  req->send(200, "application/json", "{\"code\":0,\"detail\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void WebServer::onWifiStatus(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  r.cmd = CmdID::CMD_SCAN_WIFI;
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    req->send(200, "application/json", wifiToJson(resp.wifi));
  } else {
    req->send(500, "application/json", errorJson((int)resp.code, "wifi status failed"));
  }
}

void WebServer::onWifiStatusQuick(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  r.cmd = CmdID::CMD_GET_WIFI_STATUS;
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    req->send(200, "application/json", wifiToJson(resp.wifi));
  } else {
    req->send(500, "application/json", errorJson((int)resp.code, "wifi status failed"));
  }
}

void WebServer::onWifiSet(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
  if (!data || len == 0) {
    req->send(400, "application/json", errorJson(2, "missing ssid/password"));
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (char*)data, len);
  if (err) {
    req->send(400, "application/json", errorJson(2, "invalid json"));
    return;
  }

  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_SET_WIFI;
  if (doc["ssid"].is<const char*>()) {
    String s = doc["ssid"].as<String>();
    strncpy(r.wifiCred.ssid, s.c_str(), sizeof(r.wifiCred.ssid) - 1);
  }
  if (doc["password"].is<const char*>()) {
    String p = doc["password"].as<String>();
    strncpy(r.wifiCred.password, p.c_str(), sizeof(r.wifiCred.password) - 1);
  }
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    req->send(200, "application/json", "{\"code\":0,\"detail\":\"saved\"}");
  } else {
    req->send(500, "application/json", errorJson((int)resp.code, "wifi set failed"));
  }
}

void WebServer::onWifiScan(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  r.cmd = CmdID::CMD_SCAN_WIFI;
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    // 直接返回 String 缓存, 避免 CmdResponse union 512 字节截断
    req->send(200, "application/json", api_getScanCache());
  } else {
    req->send(500, "application/json", "[]");
  }
}

void WebServer::onGetLog(AsyncWebServerRequest* req) {
  char buf[2048];
  api_getLog(buf, sizeof(buf));
  req->send(200, "text/plain", buf);
}
