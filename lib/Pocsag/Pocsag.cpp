#include "Pocsag.h"

// ---------------------------------------------------------------------------
// POCSAG 码字位结构 (bit31 = MSB, bit0 = LSB):
//   地址码字:  0 | 18-bit 地址 | 2-bit 功能码 | 10-bit BCH | P
//   消息码字:  1 | 20-bit 消息 …… | 10-bit BCH | P
//   字符集切换: 0x70→GB2312, 0xF0→ASCII, 默认GB2312模式
// ---------------------------------------------------------------------------

PocsagEncoder::PocsagEncoder() : m_bandLength(0) {
  memset(m_msgCode, 0, sizeof(m_msgCode));
  memset(m_baseband, 0, sizeof(m_baseband));
}

// ============================================================================
// 公开 API
// ============================================================================

void PocsagEncoder::encode(unsigned long addr, int function, const uint8_t *msg,
                           size_t length, DisplayMode mode, bool invert) {
  genSignal(addr, function, msg, length, mode, invert);
}

// ============================================================================
// 8-bit 字节位反转 + 掩码
//
// 中文寻呼机使用 8-bit 反转后 & 0xFE 的编码方式。
// 这确保 GB2312 双字节 (>0x80) 和 ASCII 单字节 (<0x80) 编码兼容。
//
// 例: '1' = 0x31 = 0b00110001 → 8-bit反转 = 0b10001100 = 0x8C
//                                & 0xFE = 0x8C (bit7..1 = 1000110)
//     0xB2 (GB2312) = 0b10110010 → 反转 = 0b01001101 = 0x4D
//                                  & 0xFE = 0x4C (bit7..1 = 0100110)
// ============================================================================

static uint8_t flip7bitOrder(uint8_t c) {
  // POCSAG 要求 7-bit 字符位反转 (LSB-first 传输)
  // 输入: 原始字节 (ASCII 用 bit6..bit0, GB2312 也只用 bit6..bit0)
  // 输出: 7-bit 反转值, 右对齐在 bit6..bit0
  uint8_t out = 0;
  for (int i = 0; i < 7; i++) {
    out <<= 1;
    if (c & 0x01)
      out |= 1;
    c >>= 1;
  }
  return out;
}

static uint8_t flip4bitOrder(uint8_t n) {
  // POCSAG 要求 4-bit BCD 位反转 (LSB-first 传输)
  // 例: '8' = 0x08 = 0b1000 → 反转 = 0b0001 = 0x01
  // 发射后寻呼机再反转为 0b1000 = 0x8 = '8'
  n = ((n & 0x5) << 1) | ((n & 0xA) >> 1);  // 相邻位交换
  n = ((n & 0x3) << 2) | ((n & 0xC) >> 2);  // 相邻对交换
  return n;
}

// ============================================================================
// BCH(31,21) CRC 计算
// 生成多项式: G(x) = x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1 = 0x769
// ============================================================================

unsigned long PocsagEncoder::calcCRC(unsigned long data) {
  data <<= 10;
  unsigned long mod = 0x76900000UL;
  unsigned long cmpVal = 0x40000000UL;

  while (cmpVal >= 0b10000000000UL) {
    if (data & cmpVal) {
      data ^= mod;
    }
    mod >>= 1;
    cmpVal >>= 1;
  }
  return data;
}

// ============================================================================
// 偶校验 (32-bit 码字, 校验 bit31..bit1, 结果放在 bit0)
// ============================================================================

int PocsagEncoder::evenCheck(unsigned long data) {
  int count = 0;
  for (int i = 1; i < 32; i++) {
    if (data & (1UL << i))
      count++;
  }
  return (count & 1) ? 1 : 0;
}

// ============================================================================
// 前导码: 576 bits = 18 × 32-bit 交替 1/0 模式
// ============================================================================

void PocsagEncoder::genPreamble() {
  for (int i = 0; i < 18; i++) {
    m_baseband[i] = POCSAG_PREAMBLE;
  }
}

// ============================================================================
// 地址码字生成器
// ============================================================================

unsigned long PocsagEncoder::genAddrCode(unsigned long addr, int function) {
  unsigned long code = 0;
  addr >>= 3;
  code |= (addr << 13);
  code |= ((unsigned long)(function & 0x03) << 11);
  code |= (calcCRC(code >> 11) << 1);
  code |= evenCheck(code);
  return code;
}

// ============================================================================
// 消息码字生成器
// ============================================================================

unsigned long PocsagEncoder::genMsgCode(unsigned long msgBin) {
  unsigned long code = 0x80000000UL;
  code |= ((msgBin & 0xFFFFF) << 11);
  code |= (calcCRC(code >> 11) << 1);
  code |= evenCheck(code);
  return code;
}

// ============================================================================
// 将 7-bit 反转字符打包进 20-bit 消息缓冲区 (辅助函数)
//
// 复用 deCodeAlpha 和 deCodeNumeric 中的打包逻辑。
// ============================================================================

static void packBits(unsigned long &msgBin, int &bitCount,
                     unsigned long *codeList, int &codeCount, uint8_t bits,
                     int numBits) {
  for (int b = numBits - 1; b >= 0; b--) {
    msgBin <<= 1;
    if (bits & (1 << b))
      msgBin |= 1;
    bitCount++;

    if (bitCount == 20) {
      codeList[codeCount++] = msgBin;
      msgBin = 0;
      bitCount = 0;
    }
  }
}

// ============================================================================
// Alphanumeric 消息编码 (中文寻呼机: 7-bit POCSAG + 控制字符)
//
// 中文寻呼机编码方式:
//   - 默认英文模式 (CCITT No.5, 7-bit LSB-first)
//   - \x0E (Shift Out) 切换到中文 GB2312 模式
//   - \x0F (Shift In) 切回英文模式
//   - \x04 (EOT) 消息结束符
//   - GB2312/ASCII 统一使用 7-bit 位反转编码
//   - 参考: GB2312-80, CCITT No.5
// ============================================================================

void PocsagEncoder::deCodeAlpha(const uint8_t *charBytes, size_t length) {

  memset(m_msgCode, 0, sizeof(m_msgCode));

  unsigned long codeList[MAX_CODEMORDS];
  memset(codeList, 0, sizeof(codeList));
  int codeCount = 0;

  unsigned long msgBin = 0;
  int bitCount = 0;

  // 中文寻呼机 func=3 默认进入中文模式, 遇英文需 \x0F 切到英文
  bool inGB = true;

  for (size_t i = 0; i < length; i++) {
    uint8_t c = charBytes[i];

    // GB2312 ↔ ASCII 切换
    if (c >= 0x80 && !inGB) {
      uint8_t ch = flip7bitOrder(0x0E);
      packBits(msgBin, bitCount, codeList, codeCount, ch, 7);
      inGB = true;
    } else if (c < 0x80 && inGB) {
      uint8_t ch = flip7bitOrder(0x0F);
      packBits(msgBin, bitCount, codeList, codeCount, ch, 7);
      inGB = false;
    }

    // 7-bit 位反转编码
    uint8_t ch = flip7bitOrder(c);
    packBits(msgBin, bitCount, codeList, codeCount, ch, 7);
  }

  // 消息结束符 \x04 (EOT)
  uint8_t eot = flip7bitOrder(0x04);
  packBits(msgBin, bitCount, codeList, codeCount, eot, 7);

  // 刷新剩余位 (填充 0)
  if (bitCount > 0) {
    msgBin <<= (20 - bitCount);
    codeList[codeCount++] = msgBin;
  }

  // ---- 生成消息码字 ----
  for (int i = 0; i < MAX_CODEMORDS - 1; i++) {
    if (i < codeCount) {
      m_msgCode[i] = genMsgCode(codeList[i]);
    } else {
      m_msgCode[i] = POCSAG_IDLE;
      break;
    }
  }
}

// ============================================================================
// 数字→BCD 转换
// ============================================================================

static uint8_t numericToBcd(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  switch (c) {
  // POCSAG 标准: 0xA=SPACE, 0xB='U', 0xC=(保留), 0xD='-', 0xE=')', 0xF='('
  case ' ':
    return 0xA;
  case '-':
    return 0xD;
  case 'U':
    return 0xB;
  case '[':
    return 0xF;   // '[' → '(' on pager display
  case ']':
    return 0xE;   // ']' → ')' on pager display
  default:
    return 0xA;   // unknown → SPACE
  }
}

// ============================================================================
// Numeric 消息编码 (4-bit BCD, 标准 POCSAG)
// ============================================================================

void PocsagEncoder::deCodeNumeric(const uint8_t *charBytes, size_t length) {

  memset(m_msgCode, 0, sizeof(m_msgCode));

  unsigned long codeList[MAX_CODEMORDS];
  memset(codeList, 0, sizeof(codeList));
  int codeCount = 0;

  unsigned long msgBin = 0;
  int bitCount = 0;

  for (size_t i = 0; i < length; i++) {
    uint8_t bcd = numericToBcd((char)charBytes[i]);
    bcd = flip4bitOrder(bcd);  // BCD nibble LSB-first (寻呼机做 reverseNibble)
    packBits(msgBin, bitCount, codeList, codeCount, bcd, 4);
  }

  // POCSAG 标准: 尾随填充 0xA (SPACE), 不能补零
  while (bitCount > 0 && bitCount < 20) {
    uint8_t pad = flip4bitOrder(0xA);  // SPACE 也要 LSB-first
    packBits(msgBin, bitCount, codeList, codeCount, pad, 4);
  }

  for (int i = 0; i < MAX_CODEMORDS - 1; i++) {
    if (i < codeCount) {
      m_msgCode[i] = genMsgCode(codeList[i]);
    } else {
      m_msgCode[i] = POCSAG_IDLE;
      break;
    }
  }
}

// ============================================================================
// 为一个寻呼机生成完整基带信号
//
// function 由用户通过 WebUI 的 Tone 设置指定 (A=0, B=1, C=2, D=3),
// 直接写入地址码字的功能码位, 不自动覆盖。
// ============================================================================

void PocsagEncoder::genSignal(unsigned long addr, int function,
                              const uint8_t *msg, size_t length,
                              DisplayMode mode, bool invert) {
  m_bandLength = 0;
  memset(m_baseband, 0, sizeof(m_baseband));

  // ---- 前导码 ----
  genPreamble();

  // ---- 编码消息 ----
  switch (mode) {
  case MODE_NUMERIC:
    deCodeNumeric(msg, length);
    break;
  case MODE_ALPHANUMERIC:
  default:
    deCodeAlpha(msg, length);
    break;
  }

  // ---- 计算地址所在的帧/槽位 ----
  uint8_t frameNum = addr & 0x07;
  uint8_t addrSlot = frameNum * 2;
  unsigned long addrWord = genAddrCode(addr, function);  // 使用用户指定的 Tone

  // ---- 组装批次 ----
  int msgIdx = 0;
  bool addrSent = false;
  int bandPtr = 18; // 跳过前导码 (0..17)

  // 确保末元素为 POCSAG_IDLE, 防止 while 循环因零值无限迭代
  m_msgCode[MAX_CODEMORDS - 1] = POCSAG_IDLE;

  while (!addrSent ||
         (msgIdx < MAX_CODEMORDS && m_msgCode[msgIdx] != POCSAG_IDLE)) {
    if (bandPtr >= MAX_BASEBAND)
      break;
    m_baseband[bandPtr++] = POCSAG_SYNC;

    for (int slot = 0; slot < 16; slot++) {
      if (bandPtr >= MAX_BASEBAND)
        break;
      if (slot == addrSlot && !addrSent) {
        m_baseband[bandPtr++] = addrWord;
        addrSent = true;
      } else if (addrSent &&
                 msgIdx < MAX_CODEMORDS &&
                 m_msgCode[msgIdx] != POCSAG_IDLE &&
                 m_msgCode[msgIdx] != 0) {
        m_baseband[bandPtr++] = m_msgCode[msgIdx++];
      } else {
        m_baseband[bandPtr++] = POCSAG_IDLE;
      }
    }
  }

  // ---- 尾部填充 16 个空闲码字 ----
  for (int i = 0; i < 16 && bandPtr < MAX_BASEBAND; i++) {
    m_baseband[bandPtr++] = POCSAG_IDLE;
  }

  // ---- 清除残留的零码字 ----
  for (int i = 0; i < bandPtr; i++) {
    if (m_baseband[i] == 0) {
      m_baseband[i] = POCSAG_IDLE;
    }
  }

  // ---- 反相处理 ----
  if (invert) {
    for (int i = 0; i < bandPtr; i++) {
      m_baseband[i] = ~m_baseband[i];
    }
  }

  m_bandLength = bandPtr;
}

// ============================================================================
// 批量编码 — 多地址共用一个前导码
// ============================================================================

int PocsagEncoder::encodeBatch(const uint32_t *addresses, int addrCount,
                                int function, DisplayMode mode, bool invert) {
  m_bandLength = 0;
  memset(m_baseband, 0, sizeof(m_baseband));

  if (!addresses || addrCount <= 0) return 0;

  // ---- 1. 共享前导码 ----
  genPreamble();
  int bandPtr = 18;

  // ---- 2. 逐地址生成批次 (SYNC + 16 帧) ----
  for (int i = 0; i < addrCount && bandPtr < MAX_BASEBAND - 20; i++) {
    // 2a. 编码消息 = 地址十进制字符串
    char addrStr[16];
    snprintf(addrStr, sizeof(addrStr), "%lu", (unsigned long)addresses[i]);
    size_t msgLen = strlen(addrStr);

    memset(m_msgCode, 0, sizeof(m_msgCode));
    if (mode == MODE_NUMERIC) {
      deCodeNumeric((const uint8_t *)addrStr, msgLen);
    } else {
      deCodeAlpha((const uint8_t *)addrStr, msgLen);
    }

    // 2b. 生成地址码字
    unsigned long addrWord = genAddrCode(addresses[i], function);
    uint8_t frameNum = addresses[i] & 0x07;
    uint8_t addrSlot = frameNum * 2;

    // 2c. 写入 SYNC + 16 帧
    m_baseband[bandPtr++] = POCSAG_SYNC;

    int msgIdx = 0;
    bool addrSent = false;
    for (int slot = 0; slot < 16 && bandPtr < MAX_BASEBAND; slot++) {
      if (slot == addrSlot && !addrSent) {
        m_baseband[bandPtr++] = addrWord;
        addrSent = true;
      } else if (addrSent && msgIdx < MAX_CODEMORDS &&
                 m_msgCode[msgIdx] != POCSAG_IDLE && m_msgCode[msgIdx] != 0) {
        m_baseband[bandPtr++] = m_msgCode[msgIdx++];
      } else {
        m_baseband[bandPtr++] = POCSAG_IDLE;
      }
    }
  }

  // ---- 3. 尾部填充 16 个空闲码字 ----
  for (int i = 0; i < 16 && bandPtr < MAX_BASEBAND; i++) {
    m_baseband[bandPtr++] = POCSAG_IDLE;
  }

  // ---- 4. 清除残留零码字 ----
  for (int i = 0; i < bandPtr; i++) {
    if (m_baseband[i] == 0) {
      m_baseband[i] = POCSAG_IDLE;
    }
  }

  // ---- 5. 反相处理 ----
  if (invert) {
    for (int i = 0; i < bandPtr; i++) {
      m_baseband[i] = ~m_baseband[i];
    }
  }

  m_bandLength = bandPtr;
  return bandPtr;
}
