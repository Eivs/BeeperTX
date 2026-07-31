#include "TxQueue.h"
#include <cstring>

bool TxQueue::setPendingTx(const char* msgOverride) {
  if (m_pendingTx) return false;
  m_pendingTx = true;
  if (msgOverride && msgOverride[0] != '\0') {
    strncpy(m_pendingTxMsg, msgOverride, sizeof(m_pendingTxMsg) - 1);
    m_pendingTxMsg[sizeof(m_pendingTxMsg) - 1] = '\0';
  } else {
    m_pendingTxMsg[0] = '\0';
  }
  return true;
}

bool TxQueue::consumePendingTx(char* msgOut, size_t size) {
  if (!m_pendingTx) return false;
  if (msgOut && size > 0) {
    strncpy(msgOut, m_pendingTxMsg, size - 1);
    msgOut[size - 1] = '\0';
  }
  m_pendingTx = false;
  m_pendingTxMsg[0] = '\0';
  return true;
}

bool TxQueue::setPendingWifi(const char* ssid, const char* pass) {
  if (m_pendingWifiSet) return false;
  m_pendingWifiSet = true;
  strncpy(m_pendingWifiSsid, ssid ? ssid : "", sizeof(m_pendingWifiSsid) - 1);
  m_pendingWifiSsid[sizeof(m_pendingWifiSsid) - 1] = '\0';
  strncpy(m_pendingWifiPass, pass ? pass : "", sizeof(m_pendingWifiPass) - 1);
  m_pendingWifiPass[sizeof(m_pendingWifiPass) - 1] = '\0';
  return true;
}

bool TxQueue::consumeWifi(char* ssidOut, size_t ssidSize,
                          char* passOut, size_t passSize) {
  if (!m_pendingWifiSet) return false;
  if (ssidOut && ssidSize > 0) {
    strncpy(ssidOut, m_pendingWifiSsid, ssidSize - 1);
    ssidOut[ssidSize - 1] = '\0';
  }
  if (passOut && passSize > 0) {
    strncpy(passOut, m_pendingWifiPass, passSize - 1);
    passOut[passSize - 1] = '\0';
  }
  m_pendingWifiSet = false;
  return true;
}

void TxQueue::clear() {
  m_pendingTx = false;
  m_pendingTxMsg[0] = '\0';
  m_pendingScan = false;
  m_scanCache = "[]";
  m_pendingWifiSet = false;
  m_pendingWifiSsid[0] = '\0';
  m_pendingWifiPass[0] = '\0';
}
