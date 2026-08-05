#include "TxQueue.h"
#include <cstring>

bool TxQueue::setPendingTx(const char* msgOverride) {
  // 临界区保护 check-then-act: web (AsyncTCP prio 3) 与串口 (loopMainTask
  // prio 1) 双生产者可能并发, 无保护时后到者覆盖先到者 (静默丢消息)。
  // 区内仅 strncpy, 无阻塞调用。
  taskENTER_CRITICAL(&m_pendingMux);
  if (m_pendingTx) {
    taskEXIT_CRITICAL(&m_pendingMux);
    return false;
  }
  m_pendingTx = true;
  if (msgOverride && msgOverride[0] != '\0') {
    strncpy(m_pendingTxMsg, msgOverride, sizeof(m_pendingTxMsg) - 1);
    m_pendingTxMsg[sizeof(m_pendingTxMsg) - 1] = '\0';
  } else {
    m_pendingTxMsg[0] = '\0';
  }
  taskEXIT_CRITICAL(&m_pendingMux);
  return true;
}

bool TxQueue::consumePendingTx(char* msgOut, size_t size) {
  taskENTER_CRITICAL(&m_pendingMux);
  if (!m_pendingTx) {
    taskEXIT_CRITICAL(&m_pendingMux);
    return false;
  }
  if (msgOut && size > 0) {
    strncpy(msgOut, m_pendingTxMsg, size - 1);
    msgOut[size - 1] = '\0';
  }
  m_pendingTx = false;
  m_pendingTxMsg[0] = '\0';
  taskEXIT_CRITICAL(&m_pendingMux);
  return true;
}

bool TxQueue::setPendingWifi(const char* ssid, const char* pass) {
  taskENTER_CRITICAL(&m_pendingMux);
  if (m_pendingWifiSet) {
    taskEXIT_CRITICAL(&m_pendingMux);
    return false;
  }
  m_pendingWifiSet = true;
  strncpy(m_pendingWifiSsid, ssid ? ssid : "", sizeof(m_pendingWifiSsid) - 1);
  m_pendingWifiSsid[sizeof(m_pendingWifiSsid) - 1] = '\0';
  strncpy(m_pendingWifiPass, pass ? pass : "", sizeof(m_pendingWifiPass) - 1);
  m_pendingWifiPass[sizeof(m_pendingWifiPass) - 1] = '\0';
  taskEXIT_CRITICAL(&m_pendingMux);
  return true;
}

bool TxQueue::consumeWifi(char* ssidOut, size_t ssidSize,
                          char* passOut, size_t passSize) {
  taskENTER_CRITICAL(&m_pendingMux);
  if (!m_pendingWifiSet) {
    taskEXIT_CRITICAL(&m_pendingMux);
    return false;
  }
  if (ssidOut && ssidSize > 0) {
    strncpy(ssidOut, m_pendingWifiSsid, ssidSize - 1);
    ssidOut[ssidSize - 1] = '\0';
  }
  if (passOut && passSize > 0) {
    strncpy(passOut, m_pendingWifiPass, passSize - 1);
    passOut[passSize - 1] = '\0';
  }
  m_pendingWifiSet = false;
  taskEXIT_CRITICAL(&m_pendingMux);
  return true;
}

void TxQueue::setScanCache(const char* json) {
  taskENTER_CRITICAL(&m_scanMux);
  strncpy(m_scanCache, json ? json : "[]", sizeof(m_scanCache) - 1);
  m_scanCache[sizeof(m_scanCache) - 1] = '\0';
  taskEXIT_CRITICAL(&m_scanMux);
}

void TxQueue::getScanCache(char* out, size_t size) {
  if (!out || size == 0) return;
  taskENTER_CRITICAL(&m_scanMux);
  strncpy(out, m_scanCache, size - 1);
  out[size - 1] = '\0';
  taskEXIT_CRITICAL(&m_scanMux);
}

void TxQueue::clear() {
  m_pendingTx = false;
  m_pendingTxMsg[0] = '\0';
  m_pendingScan = false;
  strncpy(m_scanCache, "[]", sizeof(m_scanCache) - 1);
  m_scanCache[sizeof(m_scanCache) - 1] = '\0';
  m_pendingWifiSet = false;
  m_pendingWifiSsid[0] = '\0';
  m_pendingWifiPass[0] = '\0';
}
