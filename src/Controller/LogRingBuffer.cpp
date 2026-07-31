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
  taskEXIT_CRITICAL(&m_mux);
}

void LogRingBuffer::get(char* buf, size_t size) {
  if (!buf || size == 0) return;
  buf[0] = '\0';

  taskENTER_CRITICAL(&m_mux);
  int start = (m_count < MAX_LINES) ? 0 : m_head;
  int remaining = (int)size - 1;
  int pos = 0;

  for (int i = 0; i < m_count && remaining > 2; i++) {
    int idx = (start + i) % MAX_LINES;
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
}

LogRingBuffer& LogRingBuffer::instance() {
  static LogRingBuffer s_log;
  return s_log;
}
