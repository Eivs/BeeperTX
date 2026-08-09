#include "ConfigStore.h"
#include <Preferences.h>
#include <cstring>

namespace {

const char*  NVS_NS      = "beepertx";   //!< NVS 命名空间 (与 WiFiManager 分离)
const char*  NVS_KEY     = "txcfg";      //!< 配置 blob 键
const uint32_t MAGIC     = 0x42545831;   //!< "BTX1"
const uint16_t VERSION   = 2;            //!< 布局版本 (v2: message[256]→[1024])

/** 落盘布局 (固定大小, 保证 getBytes 读取完整) */
struct PersistedConfig {
  uint32_t magic;
  uint16_t version;
  TxConfig cfg;
};

} // namespace

bool ConfigStore::load(TxConfig& out) {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/true)) return false;

  PersistedConfig pc;
  memset(&pc, 0, sizeof(pc));
  size_t len = prefs.getBytes(NVS_KEY, &pc, sizeof(pc));
  prefs.end();

  if (len != sizeof(pc)) return false;
  if (pc.magic != MAGIC || pc.version != VERSION) return false;
  out = pc.cfg;
  return true;
}

bool ConfigStore::save(const TxConfig& cfg) {
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return false;

  PersistedConfig pc;
  pc.magic   = MAGIC;
  pc.version = VERSION;
  pc.cfg     = cfg;
  bool ok = prefs.putBytes(NVS_KEY, &pc, sizeof(pc));
  prefs.end();
  return ok;
}

void ConfigStore::loadOrDefault(TxConfig& out) {
  if (load(out)) return;

  // 出厂默认 (与历史 api_init() 硬编码一致)
  out.encoding = EncodingType::POCSAG;
  out.phase    = PhaseType::NORMAL;
  out.tone     = ToneType::TONE_D;
  out.msgType  = MsgType::ALPHANUMERIC;
  out.freqKHz  = 152675;
  out.baudRate = 1200;
  out.address  = 1000;
  out.txPower  = 127;
  out.mailDrop = false;  // FLEX Mail Drop 默认关闭 (旧 NVS 存档因结构体变大 load 失败, 回退此默认)
  strncpy(out.message, "BeeperTX OK", sizeof(out.message) - 1);
  out.message[sizeof(out.message) - 1] = '\0';
}
