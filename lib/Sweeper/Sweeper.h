#pragma once

#include <stdint.h>
#include <stddef.h>

// 前向声明
class Transmitter;

/**
 * @brief 追码/追频 扫描模块 — 非阻塞状态机, 由 api_loop() 驱动。
 *
 * 追码 (Address Sweep): 从起始地址开始, 步进 +1, 批量拼装发射。
 *   POCSAG: 每批 50 地址, 共用一个前导码
 *   FLEX:   每批 15 地址, 帧拼接
 *
 * 追频 (Frequency Sweep): 从起始频率开始, 步进 +25kHz, 每次完整发射。
 *   频率上限 300 MHz, 到达后自动停止。
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

  static const uint32_t DEFAULT_FREQ_STEP_KHZ = 25;    //!< 默认频率步进 (kHz)
  static const uint32_t DEFAULT_FREQ_MAX_KHZ  = 300000; //!< 默认频率上限 (kHz)
  static const int    POCSAG_BATCH_SIZE = 50;           //!< POCSAG 每批地址数
  static const int    FLEX_BATCH_SIZE   = 15;           //!< FLEX 每批地址数
  static const int    SUB_BATCH_SIZE    = 8;            //!< 子批次大小 (在此粒度检查 STOP)
  static const int    BATCH_GAP_MS      = 10;           //!< 批次间隔 (ms)

  /** 发射参数 (从当前 TxConfig 继承) */
  struct Config {
    Mode     mode;
    // 追码参数
    uint32_t addrStart;
    // 追频参数
    uint32_t freqStartKHz;
    uint32_t freqStepKHz = DEFAULT_FREQ_STEP_KHZ;
    uint32_t freqMaxKHz  = DEFAULT_FREQ_MAX_KHZ;
    // 通用发射参数
    int      encoding;        // Transmitter::POCSAG / FLEX
    int      phase;           // Transmitter::NORMAL / INVERTED
    int      tone;            // Transmitter::TONE_A..D
    int      msgType;         // Transmitter::NUMERIC / ALPHANUMERIC
    int      baudRate;
    int      txPower;
    uint32_t freqKHz;         // 发射频率 (追码时用)
    uint32_t address;         // 地址 (追频时用, 固定消息内容)
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

  /** 停止扫描 */
  void stop();

  // ---- 状态查询 ----
  bool     isRunning()   const { return m_mode != IDLE; }
  Mode     mode()        const { return m_mode; }
  uint32_t current()     const { return m_current; }
  uint32_t totalSent()   const { return m_totalSent; }

  /** 每帧调用 (由 api_loop 驱动) */
  void loop();

private:
  Transmitter* m_tx;
  Mode     m_mode;
  Config   m_cfg;

  uint32_t m_current;       // 当前地址 / 当前频率 (kHz)
  uint32_t m_totalSent;     // 累计发送次数
  uint32_t m_nextTxMs;      // 下次发射时间戳 (millis)

  /** 执行一步扫描发射 */
  void sweepStep();

  /** 追码: 批量发送 (POCSAG 50/FLEX 15) */
  void txAddrBatch();

  /** 追频: 单次发射 */
  void txFreqOnce();

  /** 检查是否达到地址上限 */
  bool isAddrMax() const;

  /** 检查是否达到频率上限 */
  bool isFreqMax() const;
};
