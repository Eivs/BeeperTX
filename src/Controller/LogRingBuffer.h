#pragma once

#include <Arduino.h>   // portMUX_TYPE
#include <stddef.h>

/**
 * @brief 发射日志环形缓冲区 (线程安全, ISR/任意任务可写)。
 *
 * 从 API.cpp 抽出, 与命令调度解耦。固定容量 (50 行 × 128 字节),
 * 满时覆盖最旧条目。由 portMUX 保护, 可在中断上下文调用 add()。
 *
 * 每条日志附带单调递增的写序号 (m_seq), 支持增量导出:
 * 客户端记录上次拿到的序号, 下次只取新行, 大幅降低 /api/log 传输量
 * (sweep 高频日志场景尤为明显); 环形覆盖导致客户端错过部分行时,
 * 通过 wrapped 标记让客户端全量重绘。
 */
class LogRingBuffer {
public:
  static const int MAX_LINES = 50;
  static const int LINE_LEN  = 128;

  /** 追加一行 (自动截断到 LINE_LEN-1) */
  void add(const char* line);

  /**
   * @brief 导出为多行文本 (换行分隔, 旧→新)。兼容接口: 等同
   *        getAfter(buf, size, 0, nullptr, nullptr) 全量导出。
   * @param buf  输出缓冲区
   * @param size 缓冲区大小 (含结尾 '\0')
   */
  void get(char* buf, size_t size);

  /**
   * @brief 增量导出: 返回写序号 > after 的行 (换行分隔, 旧→新)。
   * @param buf        输出缓冲区
   * @param size       缓冲区大小 (含结尾 '\0')
   * @param after      客户端已持有的最大写序号 (0 = 全量导出)
   * @param seqOut     可选: 本次导出后的最新写序号 (客户端下轮传入 after)
   * @param wrappedOut 可选: 客户端序号已滚出环形窗口 (被覆盖), 需全量重绘
   */
  void getAfter(char* buf, size_t size, uint32_t after,
                uint32_t* seqOut, bool* wrappedOut);

  /** 当前写序号 (下一条 add 的序号) */
  uint32_t seq() const { return m_seq; }

private:
  char        m_buf[MAX_LINES][LINE_LEN];
  int         m_head  = 0;   //!< 下一条写入位置
  int         m_count = 0;   //!< 已写入条数 (≤ MAX_LINES)
  uint32_t    m_seq   = 0;   //!< 写序号 (每次 add +1; 覆盖检测靠 wrapped)
  portMUX_TYPE m_mux  = portMUX_INITIALIZER_UNLOCKED;
};
