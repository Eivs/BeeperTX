#pragma once

#include <Arduino.h>   // portMUX_TYPE
#include <stddef.h>

/**
 * @brief 发射日志环形缓冲区 (线程安全, ISR/任意任务可写)。
 *
 * 从 API.cpp 抽出, 与命令调度解耦。固定容量 (50 行 × 128 字节),
 * 满时覆盖最旧条目。由 portMUX 保护, 可在中断上下文调用 add()。
 */
class LogRingBuffer {
public:
  static const int MAX_LINES = 50;
  static const int LINE_LEN  = 128;

  /** 追加一行 (自动截断到 LINE_LEN-1) */
  void add(const char* line);

  /**
   * @brief 导出为多行文本 (换行分隔, 旧→新)。
   * @param buf  输出缓冲区
   * @param size 缓冲区大小 (含结尾 '\0')
   */
  void get(char* buf, size_t size);

  /** 全局单例 (由 CommandDispatcher 持有) */
  static LogRingBuffer& instance();

private:
  char        m_buf[MAX_LINES][LINE_LEN];
  int         m_head  = 0;   //!< 下一条写入位置
  int         m_count = 0;   //!< 已写入条数 (≤ MAX_LINES)
  portMUX_TYPE m_mux  = portMUX_INITIALIZER_UNLOCKED;
};
