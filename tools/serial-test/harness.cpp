// 宿主机测试 — 串口 SWEEP 命令解析 (修复 A: START 前缀可选; B: 参数值校验)
// stub Stream + api_dispatch, 编译真实 SerialConsole.cpp
#define private public
#include "SerialConsole.h"
#include "API.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>
#include <string>

// ---- Stream stub: 记录 println 输出 ----
std::string g_out;
class FakeStream : public Stream {
public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { (void)c; return 1; }
  size_t write(const uint8_t* b, size_t n) override {
    g_out.append((const char*)b, n);
    return n;
  }
  // Stream 需要 print(String) 等, 基类提供; println 走 write
};
FakeStream g_stream;
unsigned long g_millis = 0;
unsigned long millis() { return g_millis; }
ESPClass ESP;

// ---- api_dispatch stub: 捕获最后一次 SWEEP_START 请求 ----
bool g_dispatchCalled = false;
CmdRequest g_lastReq;
CmdResponse g_lastResp;
void api_dispatch(const CmdRequest* req, CmdResponse* resp) {
  g_dispatchCalled = true;
  g_lastReq = *req;
  memset(resp, 0, sizeof(*resp));
  resp->cmd = req->cmd;
  resp->code = RespCode::OK;
}
// 其他 API 符号 (SerialConsole.cpp 可能引用)
TxConfig api_getConfig() { TxConfig c; memset(&c, 0, sizeof(c)); c.address = 1000; c.freqKHz = 152675; return c; }
void api_log(const char*) {}
const char* api_getScanCache() { return "[{\"ssid\":\"TestAP\",\"rssi\":-45}]"; }

// ---- 断言 ----
int g_pass = 0, g_fail = 0;
void check(const char* name, bool cond) {
  if (cond) { g_pass++; printf("  PASS: %s\n", name); }
  else { g_fail++; printf("  FAIL: %s\n", name); }
}

int main() {
  SerialConsole console(g_stream);

  printf("== A: START 前缀可选 ==\n");
  // 1. 无 START
  g_dispatchCalled = false; g_out.clear();
  console.processLine("SWEEP addr");
  check("SWEEP addr 触发 dispatch", g_dispatchCalled && g_lastReq.cmd == CmdID::CMD_SWEEP_START);
  check("SWEEP addr mode=0", g_lastReq.sweepStart.mode == 0);

  // 2. 带 START
  g_dispatchCalled = false;
  console.processLine("SWEEP START addr");
  check("SWEEP START addr 触发 dispatch", g_dispatchCalled);
  check("SWEEP START addr mode=0", g_lastReq.sweepStart.mode == 0);

  // 3. 带 START freq
  g_dispatchCalled = false;
  console.processLine("SWEEP START freq");
  check("SWEEP START freq mode=1", g_dispatchCalled && g_lastReq.sweepStart.mode == 1);

  // 4. SWEEP stop (无 START)
  g_dispatchCalled = false;
  console.processLine("SWEEP stop");
  check("SWEEP stop 触发", g_dispatchCalled && g_lastReq.cmd == CmdID::CMD_SWEEP_STOP);

  printf("== B: 参数值校验 ==\n");
  // 5. 非法 baud
  g_dispatchCalled = false; g_out.clear();
  console.processLine("SWEEP START addr baud=999");
  check("baud=999 不触发 dispatch (报错)", !g_dispatchCalled);
  check("baud=999 输出 ERR", g_out.find("ERR SWEEP") != std::string::npos);

  // 6. 合法 baud
  g_dispatchCalled = false;
  console.processLine("SWEEP START addr baud=1200");
  check("baud=1200 触发 dispatch", g_dispatchCalled);
  check("baud=1200 覆盖为 1200", g_lastReq.sweepStart.baudRate == 1200);

  // 7. 非法 encoding
  g_dispatchCalled = false; g_out.clear();
  console.processLine("SWEEP START addr encoding=garbage");
  check("encoding=garbage 不触发 dispatch", !g_dispatchCalled);

  // 8. 合法 encoding=flex
  g_dispatchCalled = false;
  console.processLine("SWEEP START addr encoding=flex");
  check("encoding=flex 触发", g_dispatchCalled);
  check("encoding=flex=1", g_lastReq.sweepStart.encoding == 1);

  // 9. 非法 interval
  g_dispatchCalled = false;
  console.processLine("SWEEP START freq interval=abc");
  check("interval=abc 不触发 dispatch", !g_dispatchCalled);

  // 10. 合法 interval
  g_dispatchCalled = false;
  console.processLine("SWEEP START freq interval=1000");
  check("interval=1000 触发", g_dispatchCalled);
  check("interval=1000 值正确", g_lastReq.sweepStart.intervalMs == 1000);

  // 11. 非法 tone (多字符)
  g_dispatchCalled = false;
  console.processLine("SWEEP START addr tone=ab");
  check("tone=ab 不触发 dispatch", !g_dispatchCalled);

  // 12. 合法 tone=c
  g_dispatchCalled = false;
  console.processLine("SWEEP START addr tone=c");
  check("tone=c 触发", g_dispatchCalled);
  check("tone=c=2", g_lastReq.sweepStart.tone == 2);

  // 13. 未知 key
  g_dispatchCalled = false;
  console.processLine("SWEEP START addr bogus=1");
  check("bogus=1 不触发 dispatch", !g_dispatchCalled);

  printf("== C: SAVE 命令 (NVS 持久化入口) ==\n");
  // 14. SAVE 触发 CMD_SAVE_CONFIG
  g_dispatchCalled = false; g_out.clear();
  console.processLine("SAVE");
  check("SAVE 触发 dispatch", g_dispatchCalled && g_lastReq.cmd == CmdID::CMD_SAVE_CONFIG);
  check("SAVE 输出 OK", g_out.find("OK SAVE") != std::string::npos);

  printf("\n结果: %d PASS, %d FAIL\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
