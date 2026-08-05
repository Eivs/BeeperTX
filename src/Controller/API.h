#pragma once

/**
 * @file    API.h
 * @brief   寻呼发射系统标准控制接口定义
 *
 * 本文件定义了发射系统的命令集、数据结构及响应格式。
 * 所有外部接口 (串口控制 / WiFi Web 控制 / MQTT 等) 共享同一套命令定义，
 * 各自负责序列化/反序列化即可。
 *
 * 设计原则:
 *   - 命令 ID 枚举统一, 序列化格式由传输层决定
 *   - 串口层: 紧凑文本协议 (CMD arg1 arg2 ...)
 *   - Web 层:  JSON over HTTP / WebSocket
 *   - 新命令只需追加枚举值 + 参数结构体 + 处理器 case
 *
 * 使用方式:
 * @code
 *   #include "API.h"
 *
 *   // 传输层收到原始请求后:
 *   CmdRequest req;
 *   req.cmd = CMD_SET_CONFIG;
 *   req.setConfig.encoding = EncodingType::POCSAG;
 *   req.setConfig.freqKHz   = 153950;
 *   // ...
 *
 *   // 调度器处理:
 *   CmdResponse resp = dispatch(req);
 *
 *   // 传输层序列化 resp 返回给客户端
 * @endcode
 */

#include <stdint.h>
#include <stddef.h>
#include <cstring>
#include "ProtocolTypes.h"

// ============================================================
// 公共枚举 — 统一来自 ProtocolTypes.h (EncodingType/PhaseType/
// ToneType/MsgType), 与 Transmitter::Config / Sweeper::Config 同源,
// 消除三套枚举互相强转的问题。
// ============================================================

// ============================================================
// 命令 ID 枚举
// ============================================================

enum class CmdID : uint8_t {
  // ---- 系统 (0x00–0x0F) ----
  CMD_PING        = 0x00,   //!< 心跳检测
  CMD_GET_STATUS  = 0x01,   //!< 读取设备状态 (频率/功率/芯片信息)
  CMD_GET_CONFIG  = 0x02,   //!< 读取当前发射配置
  CMD_RESET       = 0x03,   //!< 重启设备

  // ---- 配置 (0x10–0x1F) ----
  CMD_SET_CONFIG  = 0x10,   //!< 设置发射配置 (不发射)
  CMD_SAVE_CONFIG = 0x11,   //!< 保存当前配置到 NVS/Flash

  // ---- 发射控制 (0x20–0x2F) ----
  CMD_TRANSMIT    = 0x20,   //!< 发射当前配置的消息
  CMD_ABORT       = 0x21,   //!< 中止正在进行的发射

  // ---- WiFi (0x40–0x4F) ----
  CMD_SET_WIFI       = 0x40,   //!< 设置 WiFi 凭据 (ssid + password)
  CMD_SCAN_WIFI      = 0x41,   //!< 扫描附近 WiFi 网络
  CMD_GET_WIFI_STATUS = 0x42,  //!< 查询 WiFi 状态 (不触发扫描)

  // ---- 查询 (0x30–0x3F) ----
  CMD_GET_LOG     = 0x30,   //!< 读取最近发射日志

  // ---- 扫描 (0x50–0x5F) ----
  CMD_SWEEP_START  = 0x50,  //!< 启动追码/追频
  CMD_SWEEP_STOP   = 0x51,  //!< 停止扫描
  CMD_SWEEP_STATUS = 0x52,  //!< 查询扫描状态
};

// ============================================================
// 发射配置 (可持久化)
// ============================================================

struct TxConfig {
  EncodingType encoding  = EncodingType::POCSAG;
  PhaseType    phase     = PhaseType::NORMAL;
  ToneType     tone      = ToneType::TONE_A;
  MsgType      msgType   = MsgType::ALPHANUMERIC;
  uint32_t     freqKHz   = 153950;       // 频率 (kHz), 范围 119000–1050000
  uint16_t     baudRate  = 1200;         // 码率: 512/1200/1600/2400/3200/6400
  uint32_t     address   = 1000;         // POCSAG: 0–2097151, FLEX: CAPCODE
  uint8_t      txPower   = 127;          // PA 功率 0–127 (127=+20dBm)
  char         message[256];             // UTF-8 消息, 最大 255 字节
};

// ============================================================
// 设备状态 (只读)
// ============================================================

struct DeviceStatus {
  uint16_t partId;        // Si4463 型号 (0x4463)
  uint8_t  romId;         // ROM 版本 (3=B1, 6=C2)
  uint8_t  chipRev;       // 芯片修订版本
  uint8_t  state;         // Si4463 状态机 (READY=3, TX=5, etc.)
  uint32_t freqKHz;       // 当前锁定频率 (kHz)
  uint8_t  txPower;       // 当前 PA 功率
  int16_t  rssiDbm;       // 当前 RSSI (dBm)
  uint8_t  temperatureC;  // 芯片温度 (°C)
  uint32_t uptimeS;       // 系统运行时间 (秒)
  uint32_t txCount;       // 累计发射次数
};

// ============================================================
// WiFi 状态 (只读)
// ============================================================

struct WiFiStatus {
  uint8_t  state;         // 0=DISCONNECTED, 1=CONNECTING, 2=CONNECTED, 3=AP_MODE
  char     ssid[33];      // 当前连接 SSID
  char     ip[16];        // IP 地址字符串
  int8_t   rssi;          // WiFi 信号强度 (dBm), -1 = N/A
  // 注: 扫描结果 (JSON) 不再放进 union — 体积大 (4KB) 且会撑爆任务栈。
  //     由 api_getScanCache() 独立提供, 传输层 (Web/串口) 直接引用。
};

// ============================================================
// 负载判别 (防联合体误读)
// ============================================================

/**
 * @brief CmdRequest 联合体实际负载类型。
 *
 * 传输层在填充 union 后必须同步设置 payloadType, 调度器在处理前校验,
 * 防止 "传输层填了 A 成员、处理器却读 B 成员" 的未定义行为。
 * NONE = 无参数命令 (PING/GET_STATUS/GET_CONFIG/RESET/ABORT/SCAN_WIFI/
 *        GET_WIFI_STATUS/SWEEP_STOP/SWEEP_STATUS)。
 */
enum class PayloadType : uint8_t {
  NONE         = 0,
  SET_CONFIG   = 1,   //!< union.setConfig
  TRANSMIT_MSG = 2,   //!< union.transmitMsg
  WIFI_CRED    = 3,   //!< union.wifiCred
  LOG_COUNT    = 4,   //!< union.logCount
  SWEEP_START  = 5,   //!< union.sweepStart
};

// ============================================================
// 命令请求 (传输层反序列化后填充)
// ============================================================

struct CmdRequest {
  CmdID       cmd;         // 命令 ID (必填)
  PayloadType payloadType; // 联合体实际负载 (传输层填写, 调度器校验)

  union {
    // CMD_SET_CONFIG:       完整发射配置
    TxConfig setConfig;

    // CMD_TRANSMIT:         若 message[0] != '\0' 则覆盖当前消息
    //                      否则发射已配置的消息
    char transmitMsg[256];

    // CMD_SET_WIFI:         WiFi 凭据
    struct {
      char ssid[33];
      char password[65];
    } wifiCred;

    // CMD_GET_LOG:          读取最近 N 条日志
    uint8_t logCount;

    // CMD_SWEEP_START:      启动追码/追频 (mode 必填, 其余覆盖字段 -1=未指定)
    struct {
      uint8_t mode;          // 0=追地址码, 1=追频率
      int16_t encoding;      // -1=未指定(用当前), 0=POCSAG, 1=FLEX
      int16_t baudRate;      // -1=未指定(用当前)
      int16_t phase;         // -1=未指定(用当前), 0=NORMAL, 1=INVERTED
      int16_t tone;          // -1=未指定(用当前), 0-3
      int16_t msgType;       // -1=未指定(用当前), 0=NUMERIC, 1=ALPHANUMERIC
      int32_t intervalMs;    // -1=未指定(用默认 500)
    } sweepStart;

    // CMD_PING / CMD_GET_STATUS / CMD_GET_CONFIG /
    // CMD_ABORT / CMD_SCAN_WIFI / CMD_SWEEP_STOP / CMD_SWEEP_STATUS:
    //   无额外参数
  };

  CmdRequest() { memset(this, 0, sizeof(*this)); }
};

// ============================================================
// 命令响应 (处理器填充, 传输层序列化返回)
// ============================================================

/** 响应状态码 */
enum class RespCode : uint8_t {
  OK            = 0,    //!< 成功
  ERR_UNKNOWN   = 1,    //!< 未知命令
  ERR_PARAM     = 2,    //!< 参数非法 (越界/不支持的码率等)
  ERR_BUSY      = 3,    //!< 设备忙 (正在发射中)
  ERR_HW        = 4,    //!< 硬件异常 (Si4463 无响应等)
  ERR_NOMEM     = 5,    //!< 内存不足
  ERR_ABORTED   = 6,    //!< 发射被用户中止
};

struct CmdResponse {
  CmdID    cmd;          // 回显原命令
  RespCode code;         // 执行结果
  int32_t  detail;       // 附加错误码或进度信息

  union {
    // CMD_GET_STATUS
    DeviceStatus status;

    // CMD_GET_CONFIG
    TxConfig config;

    // CMD_SCAN_WIFI / CMD_SET_WIFI
    WiFiStatus wifi;

    // CMD_SWEEP_STATUS (规格 §3.1/§9.4)
    struct {
      bool     running;
      uint8_t  mode;       // 0=idle, 1=追地址, 2=追频率
      uint32_t current;    // 当前地址/频率 (下一批起点)
      uint32_t total;      // 总步数
      uint32_t done;       // 已完成步数
      uint32_t batchStart; // 当前批次起始 (追码批量; 否则 = current)
      uint32_t batchEnd;   // 当前批次结束 (含; 否则 = current)
    } sweepStatus;
  };

  CmdResponse() { memset(this, 0, sizeof(*this)); }
};

// ============================================================
// 命令调度器接口 (由各平台实现)
// ============================================================

// 前向声明
class Transmitter;
class WiFiManager;
class Sweeper;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 执行一条命令。
 */
void api_dispatch(const CmdRequest* req, CmdResponse* resp);

/**
 * @brief 初始化 API 子系统 (加载默认配置)。
 */
void api_init();

/**
 * @brief 注入 Transmitter 指针 (由 main.cpp 在创建 tx 后调用)。
 */
void api_setTransmitter(Transmitter* tx);

/**
 * @brief 注入 WiFiManager 指针 (由 main.cpp 在创建 wifi 后调用)。
 */
void api_setWiFiManager(WiFiManager* wifi);

/**
 * @brief 注入 Sweeper 指针 (由 main.cpp 在创建 sweeper 后调用)。
 */
void api_setSweeper(Sweeper* sweeper);

/**
 * @brief 设置射频 (Si4463) 可用状态 — main.cpp 初始化失败时置 false,
 *        后台重试成功后置 true。离线期间 TX/SWEEP 命令返回 ERR_HW。
 */
void api_setRadioReady(bool ready);

/**
 * @brief Web/串口快捷入口 (规格 §8.2): 从当前 TxConfig 填充扫描配置并启动。
 * @param mode     0=追码, 1=追频
 * @param startOut 可选: 实际起始值 (addrStart / freqStartKHz)
 * @param detailOut 可选: 失败时的 detail 码 (ERR_BUSY/ERR_PARAM/ERR_HW 细分)
 * @return RespCode (OK=成功, ERR_BUSY/ERR_PARAM/ERR_HW=失败)
 */
RespCode api_sweepStartSimple(uint8_t mode, uint32_t* startOut, int* detailOut);

/**
 * @brief 获取当前发射配置的快照 (用于 Web/串口日志)。
 */
TxConfig api_getConfig();

/**
 * @brief 获取发射日志环形缓冲区 (JSON 行数组)。
 * @param buf  输出缓冲区
 * @param size 缓冲区大小
 */
void api_getLog(char* buf, size_t size);

/**
 * @brief 增量获取日志: 返回写序号 > after 的行 (换行分隔)。
 *        客户端记录上次返回的 seqOut, 下轮传入 after, 只拉新行;
 *        wrappedOut=true 表示客户端序号已滚出环形窗口, 需全量重绘。
 * @param buf        输出缓冲区
 * @param size       缓冲区大小 (含结尾 '\0')
 * @param after      客户端已持有的最大写序号 (0 = 全量)
 * @param seqOut     可选: 最新写序号
 * @param wrappedOut 可选: 环形覆盖导致行丢失
 */
void api_getLogAfter(char* buf, size_t size, uint32_t after,
                     uint32_t* seqOut, bool* wrappedOut);

/**
 * @brief 向日志缓冲区追加一行。
 */
void api_log(const char* line);

/**
 * @brief Loop 调度 — 由 main.cpp loop() 每帧调用。
 *        处理循环发射时基、状态维护、后台扫描等。
 */
void api_loop();

/**
 * @brief 触发后台 WiFi 扫描 (非阻塞)。
 */
void api_triggerScan();

/**
 * @brief 拷贝缓存的 WiFi 扫描结果 (JSON 数组字符串) 到调用方缓冲。
 * @param buf  输出缓冲区
 * @param size 缓冲区大小 (含结尾 '\0'); 结果截断到 size-1
 * 线程安全: 内部临界区读取, 不返回内部指针 (防并发重分配 use-after-free)。
 */
void api_getScanCache(char* buf, size_t size);

#ifdef __cplusplus
}
#endif

// ============================================================
// 传输层序列化提示 (参考实现, 不强制)
// ============================================================

/**
 * ┌────────────┬──────────────────────────────────┬──────────────────────────┐
 * │  传输层    │  序列化格式                       │  示例                    │
 * ├────────────┼──────────────────────────────────┼──────────────────────────┤
 * │  UART      │  文本行: <CMD> [key=val ...]\n    │ SET freq=153950 pwr=127  │
 * │  Web/JSON  │  {"cmd":"SET_CONFIG","freq":...}  │ JSON over HTTP POST      │
 * │  MQTT      │  Binary TLV + cmd ID as topic     │ topic/cmd/set_config     │
 * │  BLE       │  Binary TLV (GATT characteristic) │ 0x10 + TLV payload       │
 * └────────────┴──────────────────────────────────┴──────────────────────────┘
 *
 * 所有传输层共享同一个 CmdRequest → CmdResponse 调度路径。
 */
