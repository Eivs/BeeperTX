#pragma once

#include <stdint.h>
#include <stddef.h>
#include "Pocsag.h"
#include "ProtocolTypes.h"
#include "FlexEncoder.h"

// 前向声明
class Transmitter;

/**
 * @brief 追码/追频 扫描模块 — 非阻塞状态机, 由主循环调用 loop()。
 *
 * 追码 (Address Sweep): 从起始地址开始, 步进 +1。
 *   POCSAG: 每批 55 地址, 单前导码连续多 Batch (消息跨 SYNC), 整批一次入队
 *   FLEX:   每批 55 地址, 一次 ERS + 连续多帧 (frame=0) 多地址多消息, 整批一次入队
 *
 * 追频 (Frequency Sweep): 从起始频率开始, 步进 +25kHz, 每次完整发射。
 *   到达频率上限后自动停止, 并把频率恢复为起始值。
 *
 * 使用方式:
 * @code
 *   Sweeper sweeper;
 *   sweeper.setTransmitter(&tx);
 *   sweeper.start(cfg);
 *   // 在 loop() 中:
 *   sweeper.loop();
 * @endcode
 */
class Sweeper {
public:
  enum Mode : uint8_t {
    IDLE        = 0,
    ADDR_SWEEP  = 1,
    FREQ_SWEEP  = 2
  };

  static const uint32_t DEFAULT_FREQ_STEP_KHZ = 25;      //!< 默认频率步进 (kHz)
  static const uint32_t DEFAULT_FREQ_MAX_KHZ  = 999999;  //!< 默认频率上限 (kHz, 规格 §9.4)
  static const uint32_t DEFAULT_ADDR_END      = 0x1FFFFF; //!< 默认地址上限 (POCSAG 21-bit)
  static const int      DEFAULT_INTERVAL_MS   = 500;     //!< 默认批次间隔 (ms, 规格 §7.3)
  static const int    POCSAG_BATCH_SIZE = 55;           //!< POCSAG 每批地址数 (1 前导码连续流, ≤ MAX_BATCH 恒可编码)
  static const int    FLEX_BATCH_SIZE   = 55;           //!< FLEX 每批地址数 (一次 ERS 连续多帧; 55×6 字 ≤ 4 帧×87 字, 恒可编码)
  static const int    BATCH_GAP_MS      = 50;           //!< 批次间隔下限 (ms, 保证 WiFi/串口处理机会 + Si4463 状态恢复)

  /** 扫描配置 */
  struct Config {
    Mode     mode;
    // 追码参数
    uint32_t addrStart;
    uint32_t addrEnd   = DEFAULT_ADDR_END;   // 地址上限 (默认 POCSAG 21-bit)
    // 追频参数
    uint32_t freqStartKHz;
    uint32_t freqStepKHz = DEFAULT_FREQ_STEP_KHZ;
    uint32_t freqMaxKHz  = DEFAULT_FREQ_MAX_KHZ;
    // 通用发射参数 (统一 ProtocolTypes.h 枚举, 与 TxConfig/Transmitter 同源)
    EncodingType encoding = EncodingType::POCSAG; //!< POCSAG / FLEX
    PhaseType    phase    = PhaseType::NORMAL;    //!< NORMAL / INVERTED
    ToneType     tone     = ToneType::TONE_A;     //!< TONE_A..D
    MsgType      msgType  = MsgType::ALPHANUMERIC; //!< NUMERIC / ALPHANUMERIC
    int          baudRate;
    uint32_t freqKHz;         // 发射频率 (追码时用)
    uint32_t address;         // 地址 (追频时用, 固定消息内容)
    uint16_t intervalMs = DEFAULT_INTERVAL_MS;  // 批次/频率点间隔 (ms)
  };

  /** 扫描状态 (运行时只读, 规格 §3.1) */
  struct Status {
    bool     running = false;
    uint8_t  mode    = IDLE;   // Mode
    uint32_t current = 0;      // 当前地址/频率 (下一批起点)
    uint32_t total   = 0;      // 总步数
    uint32_t done    = 0;      // 已完成步数
    uint32_t batchStart = 0;   // 当前批次起始 (仅追码 POCSAG 批量; FLEX 单地址)
    uint32_t batchEnd   = 0;   // 当前批次结束 (含)
  };

  Sweeper();

  /** 注入 Transmitter 指针 */
  void setTransmitter(Transmitter* tx);

  /**
   * @brief 启动扫描。
   * @param cfg 扫描配置
   * @return 0=成功, -1=已在运行, -2=参数无效
   */
  int start(const Config& cfg);

  /** 停止扫描 (追频时把射频频率恢复为起始值) */
  void stop();

  // ---- 状态查询 ----
  bool     isRunning() const { return m_mode != IDLE; }
  Mode     mode()      const { return m_mode; }
  uint32_t current()   const { return m_current; }
  Status   getStatus() const;

  /** 每帧调用 (由 main.cpp loop 驱动) */
  void loop();

private:
  Transmitter* m_tx;
  Mode     m_mode;
  Config   m_cfg;

  uint32_t m_current;       // 当前地址 / 当前频率 (kHz)
  uint32_t m_total;         // 总步数 (start 时计算)
  uint32_t m_done;          // 已完成步数
  uint32_t m_origFreqKHz;   // 追频起始频率 (结束/停止时恢复)
  uint32_t m_nextTxMs;      // 下次发射时间戳 (millis)

  // 非阻塞恢复频率: stop() 只置标志 + abort, 实际恢复在 loop() 中
  // 等 txTask 完全退出 Direct TX 后执行 (避免在 AsyncTCP 回调里忙等)。
  // 快照在 stop() 时刻取值, 防止 stop 后立即 start() 覆盖 m_cfg/m_origFreqKHz。
  bool     m_restorePending = false;
  uint32_t m_restoreFreqKHz = 0;

  // POCSAG 批进度 (异步发射: 整批入队, 队列满时下帧重试)
  uint32_t m_batchBase = 0;  //!< 当前整批起始地址
  uint32_t m_batchSize = 0;  //!< 当前整批地址总数 (≤ POCSAG_BATCH_SIZE)
  uint32_t m_batchLeft = 0;  //!< 批内剩余未入队地址数

  // 整批地址缓冲 (encodeBatchMulti 输入; 一次入队整个批)
  uint32_t m_batchAddrs[POCSAG_BATCH_SIZE];

  // POCSAG 批量编码器作为实例成员 (.bss), 消除函数内 static 非重入状态
  PocsagEncoder m_batchEncoder;

  // FLEX 批量缓冲 (encodeBatch 输入; 一次入队整个批)
  FlexEncoder m_flexEncoder;                            //!< FLEX 批量编码器 (实例成员, 含 2KB 输出缓冲)
  FlexEncoder::BatchMsg m_flexBatch[FLEX_BATCH_SIZE];   //!< FLEX 批量消息描述
  char m_flexMsgText[FLEX_BATCH_SIZE][16];              //!< 地址十进制串缓冲 (≤7 位 + NUL)

  /** 计算总步数 (规格 §7.2) */
  bool calcTotalSteps();

  /** 执行一步扫描发射 */
  void sweepStep();

  /** 追码: 批量发送 (POCSAG/FLEX 各 55 地址/批) */
  void txAddrBatch();

  /** 追频: 单次发射 */
  void txFreqOnce();

  /** 检查是否达到地址上限 */
  bool isAddrMax() const;

  /** 检查是否达到频率上限 */
  bool isFreqMax() const;
};
