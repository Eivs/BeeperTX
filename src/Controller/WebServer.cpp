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

  // ---- 扫描 ----
  m_server.on("/api/sweep/start", HTTP_POST,
    [](AsyncWebServerRequest*) {},
    nullptr,
    [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
      onSweepStart(req, data, len);
    });
  m_server.on("/api/sweep/stop", HTTP_POST, [this](AsyncWebServerRequest* req) {
    onSweepStop(req);
  });
  m_server.on("/api/sweep/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
    onSweepStatus(req);
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
  doc["mailDrop"]  = cfg.mailDrop;
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
  if (doc["mailDrop"].is<bool>())
    cfg.mailDrop   = doc["mailDrop"].as<bool>();
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
  r.payloadType = PayloadType::NONE;
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
  r.payloadType = PayloadType::NONE;
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
  r.payloadType = PayloadType::NONE;
  api_dispatch(&r, &resp);

  req->send(200, "application/json", configToJson(resp.config));
}

void WebServer::onConfigSet(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
  // 先获取当前配置, 再 patch
  CmdRequest getR; CmdResponse getResp;
  memset(&getR, 0, sizeof(getR));
  getR.cmd = CmdID::CMD_GET_CONFIG;
  getR.payloadType = PayloadType::NONE;
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
  setR.payloadType = PayloadType::SET_CONFIG;
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
  r.payloadType = PayloadType::TRANSMIT_MSG;

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
    // 获取当前配置并返回给前端
    TxConfig cfg = api_getConfig();
    const char* enc = (cfg.encoding == EncodingType::POCSAG) ? "POCSAG" : "FLEX";
    const char* ph  = (cfg.phase == PhaseType::NORMAL) ? "NRM" : "INV";
    const char* tp  = (cfg.msgType == MsgType::NUMERIC) ? "NUM" : "ALPHA";
    char toneL = "ABCD"[(uint8_t)cfg.tone & 3];
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"code\":0,\"detail\":\"ok\","
             "\"enc\":\"%s\",\"phase\":\"%s\",\"type\":\"%s\","
             "\"addr\":%lu,\"freq\":%u,\"baud\":%u,\"pw\":%u,\"tone\":\"%c\"}",
             enc, ph, tp,
             (unsigned long)cfg.address,
             cfg.freqKHz,
             cfg.baudRate,
             cfg.txPower,
             toneL);
    req->send(200, "application/json", buf);
    api_log("WEB TX sent");
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
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_SCAN_WIFI;
  r.payloadType = PayloadType::NONE;
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    req->send(200, "application/json", wifiToJson(resp.wifi));
  } else {
    req->send(500, "application/json", errorJson((int)resp.code, "wifi status failed"));
  }
}

void WebServer::onWifiStatusQuick(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_GET_WIFI_STATUS;
  r.payloadType = PayloadType::NONE;
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
  r.payloadType = PayloadType::WIFI_CRED;
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
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_SCAN_WIFI;
  r.payloadType = PayloadType::NONE;
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    // 拷贝到栈缓冲再发送 (api_getScanCache 不返回内部指针, 线程安全)
    char scanBuf[4096];
    api_getScanCache(scanBuf, sizeof(scanBuf));
    req->send(200, "application/json", scanBuf);
  } else {
    req->send(500, "application/json", "[]");
  }
}

void WebServer::onGetLog(AsyncWebServerRequest* req) {
  // 增量拉取: ?after=<seq> 只返回写序号 > seq 的新行 (减少 sweep 高频
  // 日志下的传输量); 省略 after 或 after=0 全量导出。客户端序号已滚出
  // 环形窗口时 wrapped=true, 前端需全量重绘。
  uint32_t after = 0;
  if (req->hasParam("after")) {
    after = (uint32_t)strtoul(req->getParam("after")->value().c_str(),
                              nullptr, 10);
  }

  bool wrapped = false;
  uint32_t seq = 0;
  char buf[2048];
  api_getLogAfter(buf, sizeof(buf), after, &seq, &wrapped);

  JsonDocument doc;
  doc["seq"]     = seq;
  doc["wrapped"] = wrapped;
  JsonArray lines = doc["lines"].to<JsonArray>();
  char* p = buf;
  while (*p) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    if (*p) lines.add(String(p));  // 跳过空行 (环形槽位无内容)
    if (!nl) break;
    p = nl + 1;
  }

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// ============================================================
// 扫描 API 处理器
// ============================================================

void WebServer::onSweepStart(AsyncWebServerRequest* req, uint8_t* data, size_t len) {
  uint8_t mode = 0xFF;

  // 解析 JSON body: {"mode":"addr"} 或 {"mode":"freq"}
  if (data && len > 0) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, (char*)data, len);
    if (!err && doc["mode"].is<const char*>()) {
      const char* m = doc["mode"];
      if (strcmp(m, "addr") == 0)      mode = 0;
      else if (strcmp(m, "freq") == 0) mode = 1;
    }
  }

  if (mode > 1) {
    req->send(400, "application/json",
              "{\"code\":2,\"detail\":\"mode must be 'addr' or 'freq'\"}");
    return;
  }

  // 从当前 TxConfig 填充并启动 (规格 §8.2 / §9.4)
  uint32_t start = 0;
  int detail = 0;
  RespCode code = api_sweepStartSimple(mode, &start, &detail);

  if (code == RespCode::OK) {
    const char* modeName = (mode == 0) ? "addr" : "freq";
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"code\":0,\"mode\":\"%s\",\"start\":%lu}",
             modeName, (unsigned long)start);
    req->send(200, "application/json", buf);
  } else {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"code\":%d,\"detail\":%d}",
             (int)code, detail);
    req->send(400, "application/json", buf);
  }
}

void WebServer::onSweepStop(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_SWEEP_STOP;
  r.payloadType = PayloadType::NONE;
  api_dispatch(&r, &resp);

  req->send(200, "application/json",
            "{\"code\":0,\"detail\":\"stopped\"}");
}

void WebServer::onSweepStatus(AsyncWebServerRequest* req) {
  CmdRequest r; CmdResponse resp;
  memset(&r, 0, sizeof(r));
  r.cmd = CmdID::CMD_SWEEP_STATUS;
  r.payloadType = PayloadType::NONE;
  api_dispatch(&r, &resp);

  if (resp.code == RespCode::OK) {
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"code\":0,\"running\":%s,\"mode\":%d,"
             "\"current\":%lu,\"total\":%lu,\"done\":%lu,"
             "\"batchStart\":%lu,\"batchEnd\":%lu}",
             resp.sweepStatus.running ? "true" : "false",
             resp.sweepStatus.mode,
             (unsigned long)resp.sweepStatus.current,
             (unsigned long)resp.sweepStatus.total,
             (unsigned long)resp.sweepStatus.done,
             (unsigned long)resp.sweepStatus.batchStart,
             (unsigned long)resp.sweepStatus.batchEnd);
    req->send(200, "application/json", buf);
  } else {
    req->send(500, "application/json",
              "{\"code\":1,\"detail\":\"status failed\"}");
  }
}
