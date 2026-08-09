#include "LogRingBuffer.h"
#include <Arduino.h>
#include <cstring>

void LogRingBuffer::add(const char* line) {
  if (!line) return;

  taskENTER_CRITICAL(&m_mux);
  strncpy(m_buf[m_head], line, LINE_LEN - 1);
  m_buf[m_head][LINE_LEN - 1] = '\0';
  m_head = (m_head + 1) % MAX_LINES;
  if (m_count < MAX_LINES) m_count++;
  m_seq++;  // 序号单调递增; uint32 回绕 (2^32 行) 实际不可达
  taskEXIT_CRITICAL(&m_mux);
}

void LogRingBuffer::get(char* buf, size_t size) {
  getAfter(buf, size, 0, nullptr, nullptr);
}

void LogRingBuffer::getAfter(char* buf, size_t size, uint32_t after,
                             uint32_t* seqOut, bool* wrappedOut) {
  if (!buf || size == 0) return;
  buf[0] = '\0';

  bool wrapped = false;
  uint32_t outSeq = 0;

  taskENTER_CRITICAL(&m_mux);
  outSeq = m_seq;

  // 可见窗口: 最近 m_count 条。窗口内第 i 条 (0-based) 的写序号为
  // m_seq - m_count + i + 1, 故 oldest = m_seq - m_count 定义为
  // "窗口首条序号 - 1", 使 start = after - oldest 恰好是序号 > after 的
  // 首条索引; after < oldest 即客户端序号已滚出窗口 (after+1 尚未送达即
  // 被覆盖 → 真丢失), 需 wrapped 标记让前端全量重绘。
  uint32_t oldest = m_seq - m_count;
  int start = 0;
  if (after > 0) {
    if (after < oldest) {
      // 客户端序号已滚出窗口 (错过部分行): 全量导出 + wrapped 标记
      wrapped = true;
    } else {
      // 只导出 (after, m_seq] 内的行
      start = (int)(after - oldest);
    }
  }

  int startIdx = (m_count < MAX_LINES) ? 0 : m_head;
  int remaining = (int)size - 1;
  int pos = 0;

  for (int i = start; i < m_count && remaining > 2; i++) {
    int idx = (startIdx + i) % MAX_LINES;
    size_t len = strlen(m_buf[idx]);
    if (len == 0) continue;
    if (pos + (int)len + 2 > (int)size - 1) break;
    if (pos > 0) { buf[pos++] = '\n'; remaining--; }
    memcpy(buf + pos, m_buf[idx], len);
    pos += (int)len;
    remaining -= (int)len;
  }
  buf[pos] = '\0';
  taskEXIT_CRITICAL(&m_mux);

  if (seqOut)     *seqOut     = outSeq;
  if (wrappedOut) *wrappedOut = wrapped;
}
