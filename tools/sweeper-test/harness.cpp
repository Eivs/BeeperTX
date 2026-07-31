// 宿主机状态机测试 — 编译真实 Sweeper.cpp + 真实 Pocsag.cpp
#include "Sweeper.h"
#include "Transmitter.h"
#include "Pocsag.h"
#include "FlexEncoder.h"
#include "Si446x.h"
#include "API.h"
#include <Arduino.h>
#include <cstdio>
#include <cstring>

// ---- mock 实现 ----
unsigned long g_millis = 0;
FakeSerialClass Serial;

int g_sendCalls = 0;      // Transmitter::send 调用次数 (降级/追频路径)
int g_rawPocsagCalls = 0; // sendPocsagRaw 次数
int g_rawFlexCalls = 0;   // sendFlexRaw 次数
uint32_t g_freqSet[64];   // Si446x_setFrequencyKHz 调用序列
int g_freqSetCount = 0;
int g_logCount = 0;

void Transmitter::send(const Config& cfg) { (void)cfg; g_sendCalls++; }
void Transmitter::sendPocsagRaw(const unsigned long*, int, int) { g_rawPocsagCalls++; }
void Transmitter::sendFlexRaw(const uint8_t*, size_t, int, bool) { g_rawFlexCalls++; }
void Si446x_setFrequencyKHz(uint32_t f) { g_freqSet[g_freqSetCount++] = f; }
void api_log(const char*) { g_logCount++; }

// ---- 断言工具 ----
int g_pass = 0, g_fail = 0;
void check(const char* name, bool cond) {
  if (cond) { g_pass++; printf("  PASS: %s\n", name); }
  else { g_fail++; printf("  FAIL: %s\n", name); }
}

Sweeper::Config addrCfg(uint32_t start, uint32_t end) {
  Sweeper::Config c;
  c.mode = Sweeper::ADDR_SWEEP;
  c.addrStart = start;
  c.addrEnd = end;
  c.encoding = EncodingType::POCSAG;
  c.phase = PhaseType::NORMAL;
  c.tone = ToneType::TONE_A;
  c.msgType = MsgType::NUMERIC;
  c.baudRate = 1200;
  c.intervalMs = 500;
  return c;
}

Sweeper::Config freqCfg(uint32_t start, uint32_t max) {
  Sweeper::Config c;
  c.mode = Sweeper::FREQ_SWEEP;
  c.freqStartKHz = start;
  c.freqMaxKHz = max;
  c.freqStepKHz = 25;
  c.encoding = EncodingType::POCSAG;
  c.phase = PhaseType::NORMAL;
  c.tone = ToneType::TONE_A;
  c.msgType = MsgType::NUMERIC;
  c.baudRate = 1200;
  c.address = 1000;
  c.intervalMs = 500;
  return c;
}

int main() {
  printf("== 场景 1: 追码 POCSAG 批量 + total 计算 ==\n");
  {
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(1000, 1016);  // 17 步
    check("start 返回 0", sw.start(c) == 0);
    Sweeper::Status st = sw.getStatus();
    check("total = addrEnd-addrStart+1 = 17", st.total == 17);
    check("done 初始 0", st.done == 0);
    check("running", st.running);

    // 第一次 loop: 立即执行, 16 地址 (2 子批 × 8) 批量发送
    sw.loop();
    st = sw.getStatus();
    check("批量后 done=16", st.done == 16);
    check("current=1016", st.current == 1016);
    check("2 次 sendPocsagRaw (2 子批)", g_rawPocsagCalls == 2);

    // 间隔节流: 未到 intervalMs 不执行
    int doneBefore = st.done;
    sw.loop();
    check("interval 节流: 未到时间不推进", sw.getStatus().done == doneBefore);

    // 推进时间, 处理剩余 1 地址 → 完成
    g_millis += 600;
    sw.loop();
    st = sw.getStatus();
    check("完成后 done=17", st.done == 17);
    check("完成后 running=false", !st.running);
  }

  printf("== 场景 2: 6 位地址降级逐地址 ==\n");
  {
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(100000, 100002);  // 3 步, 均 >5 位
    check("start 返回 0", sw.start(c) == 0);
    sw.loop();
    check("降级走 send (3 次)", g_sendCalls == 3);
    check("done=3", sw.getStatus().done == 3);
    g_sendCalls = 0;
  }

  printf("== 场景 3: 追频 + 完成恢复频率 ==\n");
  {
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = freqCfg(152675, 152700);  // (152700-152675)/25+1 = 2 步
    check("start 返回 0", sw.start(c) == 0);
    check("total=2", sw.getStatus().total == 2);

    sw.loop();  // 发 152675
    check("第1点 done=1", sw.getStatus().done == 1);
    check("第1点 current=152700", sw.getStatus().current == 152700);

    g_millis += 600;
    sw.loop();  // 发 152700 → 完成 + 恢复
    check("第2点 done=2", sw.getStatus().done == 2);
    check("完成后 running=false", !sw.getStatus().running);
    check("恢复起始频率被调用", g_freqSetCount >= 1 &&
          g_freqSet[g_freqSetCount - 1] == 152675);
  }

  printf("== 场景 4: 手动 STOP 恢复频率 ==\n");
  {
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = freqCfg(152675, 153000);
    sw.start(c);
    g_sendCalls = 0;
    g_freqSetCount = 0;
    sw.loop();            // 发 152675 → current=152700, 设置频率 152675
    check("STOP 前 1 次发射", g_sendCalls == 1);
    check("loop 已设置起始频率", g_freqSetCount == 1 && g_freqSet[0] == 152675);
    sw.stop();            // 手动停止 → 恢复起始频率 (第 2 次设置)
    check("STOP 后恢复频率被调用", g_freqSetCount == 2 && g_freqSet[1] == 152675);
    check("STOP 后 running=false", !sw.isRunning());
    g_sendCalls = 0;
  }

  printf("== 场景 5: 参数校验 ==\n");
  {
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(2000, 1000);  // start > end
    check("addrStart>addrEnd 返回 -2", sw.start(c) == -2);
    Sweeper::Config c2 = addrCfg(1000, 1016);
    check("正常 start 返回 0", sw.start(c2) == 0);
    check("重复 start 返回 -1", sw.start(c2) == -1);
    Sweeper::Config c3 = freqCfg(110000, 150000);  // 低于 119000
    sw.stop();
    check("freq 低于下限返回 -2", sw.start(c3) == -2);
  }

  printf("\n结果: %d PASS, %d FAIL\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
