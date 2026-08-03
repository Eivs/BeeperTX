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

// 异步模拟: g_txBusy=true 模拟 txTask 发射中 (isBusy 门控);
// g_txQueueFull=true 模拟队列满 (enqueue 失败, 调用方应保留进度下帧续传)
bool g_txBusy = false;
bool g_txQueueFull = false;

bool Transmitter::send(const Config& cfg) {
  (void)cfg;
  if (g_txQueueFull) return false;
  g_sendCalls++;
  return true;
}
bool Transmitter::sendPocsagRaw(const unsigned long* bb, int wordCount, int) {
  // 与真机一致: 超 JOB_MAX_POCSAG_WORDS=1024 拒绝 (回归: 128 上限拒掉 170 CW 批量)
  if (!bb || wordCount <= 0 || wordCount > 1024) return false;
  if (g_txQueueFull) return false;
  g_rawPocsagCalls++;
  return true;
}
bool Transmitter::sendFlexRaw(const uint8_t*, size_t, int, bool) {
  if (g_txQueueFull) return false;
  g_rawFlexCalls++;
  return true;
}
bool Transmitter::isBusy() const { return g_txBusy; }
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

    // 第一次 loop: 立即执行, 17 地址整批 (1 前导码, 单次入队) 批量发送
    sw.loop();
    st = sw.getStatus();
    check("批量后 done=17", st.done == 17);
    check("current=1017", st.current == 1017);
    check("1 次 sendPocsagRaw (整批 1 前导码)", g_rawPocsagCalls == 1);

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

  printf("== 场景 2: 6 位地址全批量 (frame 7 跨 batch, 无补发) ==\n");
  {
    g_millis = 0;
    g_rawPocsagCalls = 0;
    g_sendCalls = 0;
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(100000, 100007);  // 8 步, 6 位数字消息 2 CW
    check("start 返回 0", sw.start(c) == 0);
    sw.loop();
    // 连续多 Batch 调度: frame 7 (100007) 消息跨 SYNC, 全部进批量
    check("6位地址批量: 1 次 sendPocsagRaw", g_rawPocsagCalls == 1);
    check("6位 frame7 无补发 send (0 次)", g_sendCalls == 0);
    check("done=8", sw.getStatus().done == 8);
    check("current=100008", sw.getStatus().current == 100008);
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

  printf("== 场景 6: isBusy 门控 (txTask 发射期间暂停发起下一批) ==\n");
  {
    g_millis = 0;
    g_rawPocsagCalls = 0;
    g_sendCalls = 0;
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(1000, 1016);
    sw.start(c);

    g_txBusy = true;   // 模拟 txTask 正在发射
    sw.loop();
    check("isBusy 期间不推进 (done=0)", sw.getStatus().done == 0);
    check("isBusy 期间无入队", g_rawPocsagCalls == 0);

    g_txBusy = false;  // 发射完成, 队列空闲
    sw.loop();
    check("空闲后整批 done=17", sw.getStatus().done == 17);
    check("空闲后 1 次入队", g_rawPocsagCalls == 1);
    g_txBusy = false;
  }

  printf("== 场景 7: 队列满续传 (enqueue 失败保留批内进度) ==\n");
  {
    g_millis = 0;
    g_rawPocsagCalls = 0;
    g_sendCalls = 0;
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(1000, 1016);
    sw.start(c);

    g_txQueueFull = true;  // 队列满: 首个子批 enqueue 失败
    sw.loop();
    check("队列满: 首子批未入队 done=0", sw.getStatus().done == 0);
    check("队列满: current 未推进", sw.getStatus().current == 1000);

    g_txQueueFull = false;
    g_millis += 600;       // 越过节流时间
    sw.loop();
    check("续传后整批 done=17", sw.getStatus().done == 17);
    check("续传后 current=1017", sw.getStatus().current == 1017);
    g_txQueueFull = false;
    g_millis = 0;
  }

  printf("== 场景 8: FLEX 逐地址 + 队列满重试 ==\n");
  {
    g_millis = 0;
    g_rawFlexCalls = 0;
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(5000, 5001);  // 2 步 FLEX
    c.encoding = EncodingType::FLEX;
    c.msgType = MsgType::NUMERIC;
    sw.start(c);

    g_txQueueFull = true;
    sw.loop();  // FLEX 首地址 enqueue 失败
    check("FLEX 队列满: done=0 且 current 未推进", sw.getStatus().done == 0 &&
          sw.getStatus().current == 5000);

    g_txQueueFull = false;
    g_millis += 600;
    sw.loop();  // 发 5000
    check("FLEX 第1地址 done=1", sw.getStatus().done == 1);
    check("FLEX 第1地址 current=5001", sw.getStatus().current == 5001);

    g_millis += 600;
    sw.loop();  // 发 5001 → 完成
    check("FLEX 完成后 done=2", sw.getStatus().done == 2);
    check("FLEX 完成后 running=false", !sw.getStatus().running);
    g_txQueueFull = false;
    g_millis = 0;
  }

  printf("== 场景 9: ALPHANUMERIC 批量 (frame 7 跨 batch, 无补发) ==\n");
  {
    g_millis = 0;
    g_rawPocsagCalls = 0;
    g_sendCalls = 0;
    Sweeper sw;
    Transmitter tx;
    sw.setTransmitter(&tx);
    Sweeper::Config c = addrCfg(1000, 1003);  // 4 步, 帧 0..3
    c.msgType = MsgType::ALPHANUMERIC;        // 默认: 字母消息
    sw.start(c);

    sw.loop();
    check("ALPHA: 走批量 sendPocsagRaw (1 次)", g_rawPocsagCalls == 1);
    check("ALPHA: 帧0-3 无补发 send (0 次)", g_sendCalls == 0);
    check("ALPHA: done=4", sw.getStatus().done == 4);
    check("ALPHA: current=1004", sw.getStatus().current == 1004);
    g_rawPocsagCalls = 0;
    g_sendCalls = 0;

    // 连续多 Batch 调度: frame 7 (1007) 消息跨 SYNC, 全部进批量 + 队列满续传
    Sweeper sw2;
    sw2.setTransmitter(&tx);
    Sweeper::Config c2 = addrCfg(1000, 1007);  // 8 步, 1007 是 frame 7
    c2.msgType = MsgType::ALPHANUMERIC;
    sw2.start(c2);
    g_txQueueFull = true;
    sw2.loop();
    check("ALPHA 队列满: 批量段未入队 done=0", sw2.getStatus().done == 0);
    g_txQueueFull = false;
    g_millis += 600;
    sw2.loop();
    check("ALPHA: 1 次批量 + 0 次 frame7 补发",
          g_rawPocsagCalls == 1 && g_sendCalls == 0);
    check("ALPHA: done=8", sw2.getStatus().done == 8);
    check("ALPHA: current=1008", sw2.getStatus().current == 1008);
    g_txQueueFull = false;
    g_millis = 0;
  }

  printf("\n结果: %d PASS, %d FAIL\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
