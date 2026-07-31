// BeeperTX — Si4463 POCSAG / FLEX 寻呼发射机
//
// 使用 Si4463 Direct TX Mode 通过 GPIO0 发送 POCSAG 或 FLEX 信号。
// 基于 Si446x 库 API (Zak Kemble 移植版)。
//
// == 架构 ==
//   main.cpp            — 组装层: 硬件初始化和组件编排
//   src/Controller/     — 控制层: API 调度 + Transmitter + Web + 串口
//   src/WiFiManager/    — WiFi AP/STA 管理
//   lib/Pocsag/         — POCSAG 编码 (含 GB2312 中文)
//   lib/Flex/           — FLEX 编码
//   lib/si4463/         — Si4463 SPI 射频驱动
//   lib/Charset/        — UTF-8 → GB2312 转码
//
// 硬件连接:
//   ESP32-S3 GPIO16 → Si4463 GPIO0  (TX 数据线)
//   ESP32-S3 GPIO10 → Si4463 nSEL   (SPI CS)
//   ESP32-S3 GPIO5  → Si4463 SDN    (关断引脚)
//   ESP32-S3 GPIO11 → Si4463 SDI    (SPI MOSI)
//   ESP32-S3 GPIO12 → Si4463 SCLK   (SPI SCK)
//   ESP32-S3 GPIO13 ← Si4463 SDO    (SPI MISO)
//   ESP32-S3 GPIO4  ← Si4463 IRQ    (Interrupt Output)

#include "Transmitter.h"
#include "Si446x.h"
#include "API.h"
#include "WiFiManager.h"
#include "SerialConsole.h"
#include "WebServer.h"
#include "Sweeper.h"
#include <Arduino.h>
#include <SPI.h>
#include "esp_task_wdt.h"

// ============================================================
// 引脚定义
// ============================================================
#define PIN_CS       10   // nSEL (SPI 片选)
#define PIN_SDN       5   // SDN (关断)
#define PIN_TX_DATA  16   // → Si4463 GPIO0 (TX 调制数据)
#define PIN_SCLK     12   // SPI SCK
#define PIN_MISO     13   // SPI MISO (SDO)
#define PIN_MOSI     11   // SPI MOSI (SDI)
#define PIN_NIRQ     -1   // 中断 (-1 = 轮询)


// ============================================================
// 全局组件
// ============================================================
Transmitter    s_tx(PIN_TX_DATA);
WiFiManager    s_wifi;
Sweeper        s_sweeper;
SerialConsole  s_serial(Serial);
WebServer*     s_web = nullptr;

// ============================================================
// Si4463 初始化
// ============================================================
static bool g_radioReady = false;    //!< Si4463 可用状态 (降级模式: 失败后后台重试)
static uint32_t s_radioRetryMs = 0;  //!< 上次重试时间戳
static const uint32_t RADIO_RETRY_MS = 10000;  //!< 重试间隔

static bool initSi4463() {
  Si446x_setPins(PIN_CS, PIN_SDN, PIN_NIRQ, PIN_TX_DATA,
                 PIN_SCLK, PIN_MOSI, PIN_MISO);
  pinMode(PIN_TX_DATA, OUTPUT);
  digitalWrite(PIN_TX_DATA, LOW);

  Si446x_init();

  si446x_info_t info;
  Si446x_getInfo(&info);
  if (info.part != 0x4463) {
    Serial.printf("Si4463 init FAILED: expected 0x4463, got 0x%04X\n", info.part);
    Serial.println("Check wiring & power.");
    return false;
  }

  // RF 天线开关
  Si446x_configRFswitch();

  // 初始频率和功率由 api_init() 中的默认配置决定
  // 通过 api_dispatch 的 SET_CONFIG 命令应用
  return true;
}

// ============================================================
// Si4463 后台重试 (loop 调用, 非阻塞) — 硬件故障时不挂死系统
// ============================================================
static void printSi4463Info();  // 定义见下 (重试成功后打印诊断)

static void radioRetryTick() {
  if (g_radioReady) return;
  uint32_t now = millis();
  if (now - s_radioRetryMs < RADIO_RETRY_MS) return;
  s_radioRetryMs = now;

  Serial.println(F("RADIO: retrying Si4463 init..."));
  if (initSi4463()) {
    g_radioReady = true;
    printSi4463Info();
    api_setRadioReady(true);
    Serial.println(F("RADIO: online"));
  }
}

// ============================================================
// 打印 Si4463 调试信息
// ============================================================
static void printSi4463Info() {
  Serial.println(F("\n--- Si4463 ---"));

  si446x_info_t info;
  Si446x_getInfo(&info);
  Serial.printf("  Part:   0x%04X (Si4463 = 0x4463)\n", info.part);
  Serial.printf("  ROM ID: %d (3=B1B, 6=C2A)\n", info.romId);
  Serial.printf("  Rev:    ext=%d branch=%d\n", info.revExternal, info.revBranch);

  si446x_state_t st = Si446x_getState();
  const char* sn = "?";
  switch (st) {
    case SI446X_STATE_NOCHANGE:   sn = "SLEEP(FRR=0)"; break;
    case SI446X_STATE_SLEEP:      sn = "SLEEP";        break;
    case SI446X_STATE_SPI_ACTIVE: sn = "SPI_ACTIVE";   break;
    case SI446X_STATE_READY:
    case SI446X_STATE_READY2:     sn = "READY";        break;
    case SI446X_STATE_TX:
    case SI446X_STATE_TX_TUNE:    sn = "TX";           break;
    case SI446X_STATE_RX:
    case SI446X_STATE_RX_TUNE:    sn = "RX";           break;
  }
  Serial.printf("  State:  %s (%d)\n", sn, st);
  Serial.printf("  Freq:   %u.%03u MHz\n",
                Si446x_getFrequencyKHz() / 1000,
                Si446x_getFrequencyKHz() % 1000);
  Serial.printf("  PA PWR: %d\n", Si446x_getTxPower());
  Serial.printf("  RSSI:   %d dBm\n", Si446x_getRSSI());
  Serial.println();
}

// ============================================================
// 业务主循环任务 (core 0) — 定义见文件底部 (setup 中创建)
// ============================================================
static void loopMainTask(void* arg);

// ============================================================
// setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // 发射任务 (txTask, core 1 高优先级) 会在发射期间抢占本任务 (loopTask)。
  // loopTask 的 Task WDT 订阅在此移除, 喂狗职责由 txTask 接管
  // (见 Transmitter::txTaskMain); 否则长 POCSAG 帧 (>5s) 会误触 WDT 复位。
  esp_task_wdt_delete(NULL);

  Serial.println(F("\n"
    "╔══════════════════════════════════════════╗\n"
    "║       BeeperTX — POCSAG/FLEX             ║\n"
    "╚══════════════════════════════════════════╝"));
  Serial.printf("Build: %s %s\n\n", __DATE__, __TIME__);

  Serial.printf("Chip Model: %s\n", ESP.getChipModel());
  Serial.printf("Chip Revision: %d\n", ESP.getChipRevision());

  // ---- 1. Si4463 初始化 ----
  // 失败不挂死系统: 进入降级模式 (WiFi/Web/串口仍可用), 后台每
  // RADIO_RETRY_MS 自动重试, 成功后自动恢复发射能力。
  g_radioReady = initSi4463();
  api_setRadioReady(g_radioReady);
  if (g_radioReady) {
    printSi4463Info();
  } else {
    Serial.println(F("RADIO: offline — WiFi/Web/串口仍可用; 每 10s 自动重试"));
  }

  // ---- 2. API 初始化 ----
  api_init();
  api_setTransmitter(&s_tx);
  s_sweeper.setTransmitter(&s_tx);
  api_setSweeper(&s_sweeper);

  // ---- 3. WiFi 初始化 ----
  Serial.print(F("WiFi: "));
  s_wifi.begin();
  api_setWiFiManager(&s_wifi);

  if (s_wifi.isConnected()) {
    Serial.printf("Connected to \"%s\"\n", s_wifi.ssid());
    Serial.printf("IP:    %s\n", s_wifi.ipAddress());
    Serial.printf("Host:  BeeperTX.local\n");
  } else {
    Serial.printf("AP Mode: \"%s\"\n", s_wifi.ssid());
    Serial.printf("IP:    %s\n", s_wifi.ipAddress());
  }

  // ---- 4. Web 服务器 (AP/STA 模式均启动, 用于 Captive Portal) ----
  s_web = new WebServer(80);
  if (s_web->begin()) {
    Serial.printf("Web:   http://%s/\n", s_wifi.ipAddress());
  } else {
    Serial.println(F("Web:   FAILED (check LittleFS)"));
  }

  // ---- 5. 串口控制台 ----
  s_serial.begin();
  s_serial.printf("WiFi: %s mode, IP %s\n",
    s_wifi.isConnected() ? "STA" : "AP",
    s_wifi.ipAddress());

  api_triggerScan();  // 启动后台 WiFi 扫描, 供 Web 配网使用
  api_log("BeeperTX started");
  Serial.println();

  // ---- 6. 业务主循环 (core 0) ----
  // txTask (core 1) 发射时位敲击独占 core 1, 业务 loop 若留在 Arduino
  // loopTask (core 1) 会被饿死 (Web/串口/STOP 无响应)。迁到 core 0:
  // WiFi/AsyncTCP 也跑 core 0 且优先级更高 (prio 3), 但 txTask 不再
  // 抢占本任务; 发射期间 Web/串口/STOP 全部可用。
  xTaskCreatePinnedToCore(loopMainTask, "loopMain", 10240, nullptr, 1, nullptr, 0);
}

// ============================================================
// loopMainTask — 业务主循环 (core 0, 优先级 1)
// ============================================================
static void loopMainTask(void*) {
  for (;;) {
    if (g_radioReady) {
      Si446x_SERVICE();
    } else {
      radioRetryTick();  // 射频离线: 后台重试, 不阻塞其余功能
    }
    s_wifi.loop();
    s_serial.loop();
    api_loop();
    s_sweeper.loop();  // Sweeper 非阻塞调度 (规格 §8.4)
    vTaskDelay(1);
  }
}

// ============================================================
// loop (Arduino loopTask, core 1) — 空转
// ============================================================
void loop() {
  // 业务主体已迁移到 core 0 (loopMainTask)。本函数保留空转以维持
  // Arduino 框架契约; txTask (core 1) 发射时与本函数抢占, 无业务影响。
  vTaskDelay(10);
}
