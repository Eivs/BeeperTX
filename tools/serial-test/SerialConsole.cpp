#include "SerialConsole.h"
#include "API.h"
#include <cstring>
#include <cstdlib>
#include <cstdarg>

SerialConsole::SerialConsole(Stream& stream)
  : m_stream(stream)
  , m_linePos(0)
{
  memset(m_lineBuf, 0, sizeof(m_lineBuf));
}

void SerialConsole::begin() {
  printBanner();
  m_stream.println(F("\nType HELP for available commands.\n"));
  m_stream.print(F("> "));
}

void SerialConsole::loop() {
  while (m_stream.available()) {
    char c = m_stream.read();
    if (c == '\r' || c == '\n') {
      if (m_linePos > 0) {
        m_lineBuf[m_linePos] = '\0';
        m_stream.println();  // 换行
        processLine(m_lineBuf);
        m_linePos = 0;
        m_stream.print(F("> "));
      }
    } else if (c == '\b' || c == 127) {
      if (m_linePos > 0) m_linePos--;
    } else if (m_linePos < (int)sizeof(m_lineBuf) - 1) {
      m_lineBuf[m_linePos++] = c;
    }
  }
}

void SerialConsole::writeResponse(const char* line) {
  m_stream.println(line);
}

void SerialConsole::printf(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  m_stream.print(buf);
}

// ============================================================
// 行处理
// ============================================================

void SerialConsole::processLine(const char* line) {
  // 跳过多余空格
  while (*line == ' ') line++;
  if (*line == '\0') return;

  // 提取命令名 (到空格或字符串尾)
  char cmd[16] = {0};
  const char* args = line;
  while (*args && *args != ' ') args++;
  size_t cmdLen = args - line;
  if (cmdLen >= sizeof(cmd)) cmdLen = sizeof(cmd) - 1;
  strncpy(cmd, line, cmdLen);
  while (*args == ' ') args++;  // 跳到参数

  // 分发
  if (strcasecmp(cmd, "HELP") == 0)        cmdHelp();
  else if (strcasecmp(cmd, "STATUS") == 0) cmdStatus();
  else if (strcasecmp(cmd, "CONFIG") == 0) cmdConfig();
  else if (strcasecmp(cmd, "SET") == 0)    cmdSet(args);
  else if (strcasecmp(cmd, "TX") == 0)     cmdTx(args);
  else if (strcasecmp(cmd, "WIFI") == 0)   cmdWifi(args);
  else if (strcasecmp(cmd, "SCAN") == 0)   cmdScan();
  else if (strcasecmp(cmd, "SWEEP") == 0)  cmdSweep(args);
  else if (strcasecmp(cmd, "STOP") == 0)   cmdStop();
  else if (strcasecmp(cmd, "RESET") == 0)  cmdReset();
  else if (strcasecmp(cmd, "PING") == 0)   cmdPing();
  else {
    m_stream.print(F("ERR unknown command: "));
    m_stream.println(cmd);
  }
}

// ============================================================
// 启动横幅
// ============================================================

void SerialConsole::printBanner() {
  m_stream.println(F("\n"
    "==========================================\n"
    "   BeeperTX — Si4463 POCSAG/FLEX TX\n"
    "=========================================="));
}

// ============================================================
// HELP
// ============================================================

void SerialConsole::printHelp() {
  m_stream.println(F(
    "\n"
    "=== BeeperTX Serial Commands ===\n"
    "\n"
    "  HELP                  Show this help\n"
    "  STATUS                Device status (chip, freq, power, IP)\n"
    "  CONFIG                Current TX config\n"
    "  SET key=value ...     Set TX parameters\n"
    "     freq=<kHz>         Frequency (119000-1050000)\n"
    "     pwr=<0-127>        TX power\n"
    "     baud=<rate>        Baud rate (512/1200/1600/2400/3200/6400)\n"
    "     addr=<n>           Pager address/capcode\n"
    "     encoding=pocsag|flex\n"
    "     phase=norm|inv     Phase (normal/inverted)\n"
    "     tone=a|b|c|d       POCSAG tone\n"
    "     msgType=num|alpha  Numeric / Alphanumeric\n"
    "     msg=<text>         Message content (UTF-8)\n"
    "  TX [msg=<text>]       Transmit once (optional msg override)\n"
    "  SWEEP [START] addr|freq [encoding=pocsag|flex] [baud=<rate>]\n"
    "       [phase=norm|inv] [tone=a..d] [msgType=num|alpha] [interval=<ms>]\n"
    "  SWEEP [START] stop|status  Stop sweep / show progress\n"
    "  STOP                   Alias for SWEEP stop\n"
    "  WIFI                  Show WiFi status\n"
    "  WIFI ssid=X pass=Y    Set WiFi credentials (triggers reboot)\n"
    "  SCAN                  Scan nearby WiFi networks\n"
    "  RESET                 Reboot device\n"
    "  PING                  Heartbeat\n"
  ));
}

// ============================================================
// 各命令实现
// ============================================================

void SerialConsole::cmdHelp() { printHelp(); }

void SerialConsole::cmdPing() {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_PING;
  api_dispatch(&req, &resp);
  m_stream.print(F("OK PING uptime="));
  m_stream.println(resp.detail);
}

void SerialConsole::cmdStatus() {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_GET_STATUS;
  api_dispatch(&req, &resp);

  if (resp.code != RespCode::OK) {
    m_stream.print(F("ERR STATUS code="));
    m_stream.println((int)resp.code);
    return;
  }

  DeviceStatus& s = resp.status;
  m_stream.print(F("OK STATUS "));
  m_stream.print(F("part=0x")); m_stream.print(s.partId, HEX);
  m_stream.print(F(" rom="));   m_stream.print(s.romId);
  m_stream.print(F(" rev="));   m_stream.print(s.chipRev);
  m_stream.print(F(" state=")); m_stream.print(s.state);
  m_stream.print(F(" freq="));  m_stream.print(s.freqKHz);
  m_stream.print(F("kHz pwr="));m_stream.print(s.txPower);
  m_stream.print(F(" rssi="));  m_stream.print(s.rssiDbm);
  m_stream.print(F("dBm uptime="));m_stream.print(s.uptimeS);
  m_stream.print(F("s tx="));   m_stream.print(s.txCount);
  m_stream.println();
}

void SerialConsole::cmdConfig() {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_GET_CONFIG;
  api_dispatch(&req, &resp);

  TxConfig& c = resp.config;
  m_stream.print(F("OK CONFIG "));
  m_stream.print(F("encoding="));
  m_stream.print(c.encoding == EncodingType::POCSAG ? "pocsag" : "flex");
  m_stream.print(F(" phase="));
  m_stream.print(c.phase == PhaseType::NORMAL ? "norm" : "inv");
  m_stream.print(F(" tone="));
  m_stream.print((char)('A' + (int)c.tone));
  m_stream.print(F(" msgType="));
  m_stream.print(c.msgType == MsgType::NUMERIC ? "num" : "alpha");
  m_stream.print(F(" freq=")); m_stream.print(c.freqKHz);
  m_stream.print(F("kHz pwr="));m_stream.print(c.txPower);
  m_stream.print(F(" baud=")); m_stream.print(c.baudRate);
  m_stream.print(F(" addr=")); m_stream.print(c.address);
  m_stream.print(F(" msg=\""));m_stream.print(c.message);
  m_stream.println(F("\""));
}

void SerialConsole::cmdSet(const char* args) {
  if (!args || *args == '\0') {
    m_stream.println(F("ERR SET: need key=value pairs. See HELP."));
    return;
  }

  // 先获取当前配置
  CmdRequest getReq; CmdResponse getResp;
  memset(&getReq, 0, sizeof(getReq));
  getReq.cmd = CmdID::CMD_GET_CONFIG;
  api_dispatch(&getReq, &getResp);
  TxConfig cfg = getResp.config;

  // 解析 key=value 对
  char buf[256];
  strncpy(buf, args, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* token = strtok(buf, " ");
  while (token) {
    char* eq = strchr(token, '=');
    if (!eq) { token = strtok(nullptr, " "); continue; }
    *eq = '\0';
    const char* key = token;
    const char* val = eq + 1;

    if (strcasecmp(key, "freq") == 0)       cfg.freqKHz = atol(val);
    else if (strcasecmp(key, "pwr") == 0)   cfg.txPower = atoi(val);
    else if (strcasecmp(key, "baud") == 0)  cfg.baudRate = atoi(val);
    else if (strcasecmp(key, "addr") == 0)  cfg.address = atoll(val);
    else if (strcasecmp(key, "encoding") == 0) {
      cfg.encoding = (strcasecmp(val, "flex") == 0)
                     ? EncodingType::FLEX : EncodingType::POCSAG;
    }
    else if (strcasecmp(key, "phase") == 0) {
      cfg.phase = (strcasecmp(val, "inv") == 0)
                  ? PhaseType::INVERTED : PhaseType::NORMAL;
    }
    else if (strcasecmp(key, "tone") == 0) {
      char t = val[0] | 32;  // 小写
      if (t >= 'a' && t <= 'd') cfg.tone = static_cast<ToneType>(t - 'a');
    }
    else if (strcasecmp(key, "msgType") == 0) {
      cfg.msgType = (strcasecmp(val, "num") == 0)
                    ? MsgType::NUMERIC : MsgType::ALPHANUMERIC;
    }
    else if (strcasecmp(key, "msg") == 0) {
      strncpy(cfg.message, val, sizeof(cfg.message) - 1);
      cfg.message[sizeof(cfg.message) - 1] = '\0';
    }
    token = strtok(nullptr, " ");
  }

  // 提交
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_SET_CONFIG;
  req.setConfig = cfg;
  api_dispatch(&req, &resp);

  if (resp.code == RespCode::OK) {
    m_stream.println(F("OK SET"));
    api_log("SER SET OK");
  } else {
    m_stream.print(F("ERR SET code="));
    m_stream.print((int)resp.code);
    m_stream.print(F(" detail="));
    m_stream.println(resp.detail);
  }
}

void SerialConsole::cmdTx(const char* args) {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_TRANSMIT;

  // 可选消息覆盖
  if (args && *args) {
    const char* p = strstr(args, "msg=");
    if (p) {
      p += 4;
      // 截取到下一个空格或行尾
      const char* end = strchr(p, ' ');
      size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len >= sizeof(req.transmitMsg)) len = sizeof(req.transmitMsg) - 1;
      memcpy(req.transmitMsg, p, len);
      req.transmitMsg[len] = '\0';
    }
  }

  api_dispatch(&req, &resp);

  if (resp.code == RespCode::OK) {
    // api_loop() 会在发送前通过 api_log() 打印完整参数,
    // 此处只需回显确认
    m_stream.print(F("OK TX count="));
    m_stream.println(resp.detail);
  } else {
    m_stream.print(F("ERR TX code="));
    m_stream.println((int)resp.code);
  }
}

void SerialConsole::cmdWifi(const char* args) {
  // 带参数 = 设置 WiFi
  if (args && strstr(args, "ssid=")) {
    CmdRequest req; CmdResponse resp;
    memset(&req, 0, sizeof(req));
    req.cmd = CmdID::CMD_SET_WIFI;

    // 解析 ssid=... pass=...
    const char* s = strstr(args, "ssid=");
    if (s) {
      s += 5;
      const char* end = strchr(s, ' ');
      size_t len = end ? (size_t)(end - s) : strlen(s);
      if (len >= sizeof(req.wifiCred.ssid)) len = sizeof(req.wifiCred.ssid) - 1;
      strncpy(req.wifiCred.ssid, s, len);
      req.wifiCred.ssid[len] = '\0';
    }
    const char* p = strstr(args, "pass=");
    if (p) {
      p += 5;
      const char* end = strchr(p, ' ');
      size_t len = end ? (size_t)(end - p) : strlen(p);
      if (len >= sizeof(req.wifiCred.password)) len = sizeof(req.wifiCred.password) - 1;
      strncpy(req.wifiCred.password, p, len);
      req.wifiCred.password[len] = '\0';
    }

    api_dispatch(&req, &resp);

    if (resp.code == RespCode::OK) {
      m_stream.println(F("OK WIFI saved, connecting..."));
      m_stream.println(F("Device will reboot in STA mode if connection succeeds."));
    } else {
      m_stream.print(F("ERR WIFI code="));
      m_stream.println((int)resp.code);
    }
    return;
  }

  // 无参数 = 查询 WiFi 状态 (不触发扫描)
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_GET_WIFI_STATUS;
  api_dispatch(&req, &resp);

  if (resp.code == RespCode::OK) {
    const char* stateNames[] = {"DISCONNECTED", "CONNECTING", "CONNECTED", "AP"};
    m_stream.print(F("OK WIFI state="));
    m_stream.print(resp.wifi.state < 4 ? stateNames[resp.wifi.state] : "?");
    m_stream.println();
  }
}

void SerialConsole::cmdScan() {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_SCAN_WIFI;
  api_dispatch(&req, &resp);

  if (resp.code == RespCode::OK) {
    m_stream.print(F("OK SCAN "));
    m_stream.println(resp.wifi.scanResult);
  }
}

void SerialConsole::cmdSweep(const char* args) {
  // 语法: SWEEP [START] addr|freq [encoding=..] [baud=..] [phase=..] [tone=..]
  //                  [msgType=..] [interval=..]
  //        SWEEP [START] stop|status
  // "START" 前缀可选, 兼容两种写法 (规格 §8.3 附录 A 与 help)。
  if (!args) {
    m_stream.println(F("ERR SWEEP: need 'addr', 'freq', 'stop' or 'status'. See HELP."));
    return;
  }
  while (*args == ' ') args++;

  // ---- 跳过可选 "START" 前缀 ----
  if (strncasecmp(args, "start", 5) == 0 &&
      (args[5] == ' ' || args[5] == '\0')) {
    args += 5;
    while (*args == ' ') args++;
  }

  // ---- STOP / STATUS ----
  if (strcasecmp(args, "stop") == 0) {
    cmdStop();
    return;
  }
  if (strcasecmp(args, "status") == 0) {
    cmdSweepStatus();
    return;
  }

  // ---- START <mode> [params] ----
  char buf[128];
  strncpy(buf, args, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* tok = strtok(buf, " ");
  uint8_t mode = 0xFF;
  if (tok) {
    if (strcasecmp(tok, "addr") == 0)      mode = 0;
    else if (strcasecmp(tok, "freq") == 0) mode = 1;
  }
  if (mode > 1) {
    m_stream.println(F("ERR SWEEP: need 'addr' or 'freq'. See HELP."));
    return;
  }

  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_SWEEP_START;
  req.sweepStart.mode = mode;
  // 默认全部覆盖字段为 -1 (用当前配置/默认值)
  req.sweepStart.encoding   = -1;
  req.sweepStart.baudRate   = -1;
  req.sweepStart.phase      = -1;
  req.sweepStart.tone       = -1;
  req.sweepStart.msgType    = -1;
  req.sweepStart.intervalMs = -1;

  // 解析参数覆盖 (规格 §8.3 附录 A), 非法 key 或非法值均报错
  bool badParam = false;
  char badKey[24] = "";
  while ((tok = strtok(nullptr, " ")) != nullptr) {
    char* eq = strchr(tok, '=');
    if (!eq) continue;
    *eq = '\0';
    const char* key = tok;
    const char* val = eq + 1;

    if (strcasecmp(key, "encoding") == 0) {
      if (strcasecmp(val, "pocsag") == 0)      req.sweepStart.encoding = 0;
      else if (strcasecmp(val, "flex") == 0)   req.sweepStart.encoding = 1;
      else { badParam = true; strncpy(badKey, key, sizeof(badKey) - 1); }
    } else if (strcasecmp(key, "baud") == 0) {
      int b = atoi(val);
      if (b == 512 || b == 1200 || b == 1600 || b == 2400 ||
          b == 3200 || b == 6400) {
        req.sweepStart.baudRate = b;
      } else { badParam = true; strncpy(badKey, key, sizeof(badKey) - 1); }
    } else if (strcasecmp(key, "phase") == 0) {
      if (strcasecmp(val, "norm") == 0)        req.sweepStart.phase = 0;
      else if (strcasecmp(val, "inv") == 0)    req.sweepStart.phase = 1;
      else { badParam = true; strncpy(badKey, key, sizeof(badKey) - 1); }
    } else if (strcasecmp(key, "tone") == 0) {
      char t = val[0] | 32;
      if (t >= 'a' && t <= 'd' && val[1] == '\0') {
        req.sweepStart.tone = t - 'a';
      } else { badParam = true; strncpy(badKey, key, sizeof(badKey) - 1); }
    } else if (strcasecmp(key, "msgType") == 0) {
      if (strcasecmp(val, "num") == 0)         req.sweepStart.msgType = 0;
      else if (strcasecmp(val, "alpha") == 0)  req.sweepStart.msgType = 1;
      else { badParam = true; strncpy(badKey, key, sizeof(badKey) - 1); }
    } else if (strcasecmp(key, "interval") == 0) {
      int iv = atoi(val);
      if (iv > 0 && iv <= 60000) {
        req.sweepStart.intervalMs = iv;
      } else { badParam = true; strncpy(badKey, key, sizeof(badKey) - 1); }
    } else {
      badParam = true; strncpy(badKey, key, sizeof(badKey) - 1);
    }
  }

  if (badParam) {
    m_stream.print(F("ERR SWEEP: invalid parameter '"));
    m_stream.print(badKey[0] ? badKey : "?");
    m_stream.println(F("'. See HELP."));
    return;
  }

  api_dispatch(&req, &resp);

  if (resp.code == RespCode::OK) {
    const char* modeName = (mode == 0) ? "addr" : "freq";
    TxConfig cfg = api_getConfig();
    m_stream.print(F("OK SWEEP mode="));
    m_stream.print(modeName);
    if (mode == 0) {
      m_stream.print(F(" start="));
      m_stream.print(cfg.address);
    } else {
      m_stream.print(F(" start="));
      m_stream.print(cfg.freqKHz);
      m_stream.print(F("kHz step=25kHz max=999999kHz"));
    }
    m_stream.println();
  } else {
    m_stream.print(F("ERR SWEEP code="));
    m_stream.print((int)resp.code);
    m_stream.print(F(" detail="));
    m_stream.println(resp.detail);
  }
}

void SerialConsole::cmdSweepStatus() {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_SWEEP_STATUS;
  api_dispatch(&req, &resp);

  if (resp.code != RespCode::OK) {
    m_stream.print(F("ERR SWEEP STATUS code="));
    m_stream.println((int)resp.code);
    return;
  }

  m_stream.print(F("OK SWEEP "));
  if (resp.sweepStatus.running) {
    m_stream.print(resp.sweepStatus.mode == 1 ? "running mode=addr" :
                   resp.sweepStatus.mode == 2 ? "running mode=freq" : "running");
    m_stream.print(F(" current="));
    m_stream.print(resp.sweepStatus.current);
  } else {
    m_stream.print(F("idle"));
  }
  m_stream.print(F(" done="));
  m_stream.print(resp.sweepStatus.done);
  m_stream.print('/');
  m_stream.print(resp.sweepStatus.total);
  m_stream.println();
}

void SerialConsole::cmdStop() {
  CmdRequest req; CmdResponse resp;
  memset(&req, 0, sizeof(req));
  req.cmd = CmdID::CMD_SWEEP_STOP;
  api_dispatch(&req, &resp);

  if (resp.code == RespCode::OK) {
    m_stream.println(F("OK STOP sweep stopped"));
  } else {
    m_stream.print(F("ERR STOP code="));
    m_stream.println((int)resp.code);
  }
}

void SerialConsole::cmdReset() {
  m_stream.println(F("OK RESET rebooting..."));
  delay(100);
  ESP.restart();
}
