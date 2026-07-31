# Sweeper 优化计划（feat/sweep 审查整改）

> 日期：2026-07-31
> 基线：`origin/main`（merge-base `d0e04d2`），分支 `feat/sweep`（2 个提交）
> 依据：`docs/Sweeper-设计文档.md` v3（历史恢复）+ `CLAUDE.md` 架构标准
> 目标：修复双轴审查发现的全部问题，使实现符合规格 v3 与项目分层约定

---

## 0. 问题清单 → 阶段映射

| # | 审查发现 | 阶段 |
|---|----------|------|
| c1 | `encodeBatch` 每地址一组帧，而非规格 §4.2 的 8 地址共享一组 | P1 |
| c2 | `addr&0x07==7` 时 2-CW 消息被截断 | P1 |
| s4 | `POCSAG_BATCH_SIZE=50` 与 `MAX_BASEBAND=768` 不符（潜伏截断） | P1 |
| s1 | `lib/Sweeper/` 依赖倒置（含 `src/Controller` 头文件） | P2 |
| s2 | 绕过 `Transmitter::send` 标准路径、`sendFlexRaw` 重复 invert 逻辑 | P2 |
| s3 | FLEX 批 15 地址 → 单批最长约 28 s 忙等待阻塞 | P2 |
| a1 | 缺 `intervalMs`（`BATCH_GAP_MS=10` 硬编码） | P3 |
| a3 | Status 缺 `total`/`done`，`calcTotalSteps()` 未实现 | P3 |
| c3 | 地址上限 0x1FFFFF vs 规格 9999999（规格错误，见决策 D1）；频率上限 300 MHz vs 999999 kHz | P3 |
| c4 | 追频结束不恢复原频率 | P3 |
| — | `Sweeper::Config.txPower` 为死字段（`Transmitter::Config` 无此字段） | P3 |
| a2 | 串口缺 `SWEEP STATUS` / `SWEEP STOP`，无参数覆盖 | P4 |
| a5 | `api_sweepStartSimple()` 未抽出 | P4 |
| a6 | `loop()` 由 `api_loop()` 驱动，规格要求 `main.cpp` | P4 |
| b2 | 裸 `STOP` 命令（保留为别名，文档化） | P4 |
| c5 | 前端 `mode===1` else 分支把 IDLE 显示为 "Freq" | P5 |
| a4 | `TX_SETTLE_MS` 10→2 ms（规格 §10.1） | P6 |
| b1/b3 | ERR_BUSY 保护、confirm 对话框（保留，文档化） | P6 |

---

## 1. 关键决策记录（含规格订正）

| 编号 | 决策 | 理由 |
|------|------|------|
| D1 | 地址上限采用 POCSAG 21-bit 硬上限 `0x1FFFFF`（2,097,151），规格 §3.1 的 `addrEnd=9999999` 超出地址空间，判定为规格笔误。`Config` 增加 `addrEnd` 字段，默认 `0x1FFFFF`，可配置 | POCSAG 地址码字只有 21 bit；`genAddrCode` 使用 `addr >>= 3` 装入 18 bit，物理不可达 9999999 |
| D2 | 批量编码仅支持 ≤1-CW 消息（≤5 位数字）；6–7 位地址自动降级为逐地址发送（batchSize=1 语义）。规格 §13.1 已提供降级机制 | `deCodeNumeric` 每码字 20 bit = 5 位 BCD；2-CW 消息会侵占相邻地址帧槽（c2 根因） |
| D3 | `POCSAG_BATCH_SIZE` 由 50 改为 **16**（= 2 组 × 8 地址，共享 1 个前导码），`SUB_BATCH_SIZE=8` 保留作 STOP 检查粒度；组内结构严格按规格 §4.2 | 50 非 8 的倍数且超出 `MAX_BASEBAND` 容量（s4）；16 地址/批 ≈ 6.75 地址/s，与规格 §11.1 估算一致 |
| D4 | `FLEX_BATCH_SIZE` 由 15 改为 **1**（规格 §5.2 明确 FLEX 每批 1 地址；flex.h 不支持拼接，§5.1） | 消除单批约 28 s 忙等待阻塞（s3）；header 注释"帧拼接"本就是不实描述 |
| D5 | 频率上限 `freqMaxKHz` 默认 **999999**（规格 §9.4 与附录 A），保留硬件 clamp（Si4463 上限约 1050 MHz） | 999.999 MHz 在硬件能力内 |
| D6 | `Sweeper` 从 `lib/Sweeper/` 移至 **`src/Controller/Sweeper.{h,cpp}`** | CLAUDE.md「项目架构」四层约定优先于规格 §12.1 的路径（规格路径即分层违规）；控制层依赖 Transmitter/API 属正常 |
| D7 | `intervalMs`（默认 500）作为**批次间**节流；子批次间保留 `BATCH_GAP_MS=10` 让出 WiFi/串口（规格 §13.2 缓解） | 规格 §7.3：POCSAG 300–500 ms，FLEX/追频 500–1000 ms |
| D8 | `Status = {running, mode, current, total, done}`，移除 `totalSent`（规格 §3.1/§9.4 无此字段） | 规格优先；前端同步 |
| D9 | 追频完成/停止时把频率恢复为起始频率（`start()` 时快照） | c4；规格未明说但符合"不破坏现有发射功能"（§1.2 设计目标） |
| D10 | 范围蔓延项（ERR_BUSY 阻止 SET_CONFIG/TRANSMIT、confirm 对话框、裸 `STOP` 别名）**保留**，在 CLAUDE.md 文档化 | 合理安全措施，撤销无益 |

---

## Phase 0：基线确认（约 0.5 h）

**内容**：执行前逐项确认以下事实，防止计划与代码脱节。

1. `lib/Pocsag/Pocsag.cpp`：`genPreamble()`（18 CW）、`genAddrCode(addr, func)`、`deCodeNumeric`（5 位 BCD/码字）、`deCodeAlpha`、`genMsgCode`、常量 `MAX_BASEBAND=768` / `MAX_CODEMORDS=512` / `POCSAG_SYNC=0x7CD215D8` / `POCSAG_IDLE=0x7A89C197` / `POCSAG_PREAMBLE=0xAAAAAAAA`
2. `src/Controller/Transmitter.h`：`Config{encoding, phase, baudRate, tone, address(uint64_t), message[256], msgType}`（**无 txPower**）；`bitTimeUs()`、`sendPocsagRaw()`、`sendFlexRaw()`
3. `lib/Flex/FlexEncoder.h`：`encode(uint64_t, const char*, MsgType)`、`getData()`、`hasError()`
4. `src/Controller/API.cpp`：`s_sweeper` 注入点、`api_dispatch` 表、`api_loop()` 内的 `s_sweeper->loop()` 调用
5. `src/main.cpp`：`loop()` 顺序（`Si446x_SERVICE → wifi → serial → api_loop`），确认 `s_sweeper.loop()` 插入点
6. `data/js/main.js` / `config-panel.js`：轮询与 `updateSweepUI` 现状

**验证**：`git status` 干净；`pio run` 基线编译通过。
**反模式防护**：不在此阶段改任何代码。

---

## Phase 1：POCSAG 批量编码修复（核心，约 4 h）

**目标**：修复 c1（帧结构）、c2（消息截断）、s4（常量/容量），重写 `encodeBatch` 为规格 §4.2/§4.5 的结构。

**实现内容**（`lib/Pocsag/Pocsag.h` / `Pocsag.cpp`）：

1. **重写 `encodeBatch()`**，按规格 §4.2 帧结构：
   - 组 = `[SYNC] + 8 帧 × 2 码字`，每组最多 8 地址，地址按 `addr & 0x07` 放入对应帧槽（规格 §13.1：地址必须落在正确帧）
   - 布局：组 g 的地址 `addrs[g*8 .. g*8+7]`，帧 f 槽 0 = `genAddrCode(addr, func)`，槽 1 = 该地址的**单条消息码字**（规格 §4.5 伪代码）
   - 组尾（不足 8 地址的组）帧填 `IDLE×2`；整批结束后补 16 个 `IDLE`（规格 §4.2）
   - 前导码仍为 18 个 `POCSAG_PREAMBLE`，整批共享 1 个（规格 §11.3 提速来源）
   - `invert` 参数行为不变（对所有码字取反）
2. **消息 1-CW 校验（c2 修复，决策 D2）**：
   - 编码前计算消息码字数：`MODE_NUMERIC` 下 ≤5 位数字 = 1 CW；`MODE_ALPHANUMERIC` 下需 ≤20 bit 有效位（含控制字符与 EOT）
   - 消息 >1 CW 的地址**不进批量**，由调用方降级逐地址发送
   - `encodeBatch` 返回实际编码的地址数（签名不变仍返回码字数时，新增 `int* addrEncoded` 或返回结构），调用方据此推进 `m_current`
3. **容量防护（s4）**：`encodeBatch` 内部按 `MAX_BASEBAND` 计算可容纳的最大组数，超限返回实际值而非静默截断；批内地址数由调用方保证 ≤ `POCSAG_BATCH_SIZE`
4. **`POCSAG_BATCH_SIZE` 改为 16**（决策 D3），`Sweeper.h` 注释同步更新为"2 组 × 8 地址共享前导码"
5. **符号整理**：确认 `genSignal()` 现有调用不受影响；`encodeBatch` 与 `genSignal` 共用 `genPreamble/genAddrCode/deCode*`

**文档引用**：规格 §4.2（帧结构图）、§4.5（伪代码）、§13.1（降级机制）、§11.3（提速依据）；`lib/Pocsag/Pocsag.h:15-16`（容量常量）。
**验证清单**：
- 主机端验证脚本（新增 `tools/verify_pocsag_batch.py` 或 `.scratch/`，读取 `encodeBatch` 输出）：断言 ① 前 18 CW 为 preamble；② 每 17 CW 一组以 SYNC 开头；③ 组内第 `addr&7` 帧槽 0 为对应地址码字（可解回 `addr`）；④ 消息槽为合法消息码字；⑤ 尾 16 个 IDLE；⑥ ≤5 位地址全 1-CW、6–7 位地址返回降级标记
- `pio run` 编译通过
- 串口实测：追码一批，日志显示批次范围与降级地址
**反模式防护**：
- 不引入规格没有的"消息区/地址区分离"布局（接收机按地址后紧跟消息解析，破坏该顺序会丢消息）
- 不在 `encodeBatch` 内新增 `addrEnd` 等参数（容量逻辑留在调用方）
- 不把 `genSignal` 与 `encodeBatch` 合并重构（`genSignal` 是既有稳定路径）

---

## Phase 2：架构分层与发射接口收敛（约 2 h）

**目标**：修复 s1（依赖倒置）、s2（绕过接口）、s3（FLEX 阻塞）。

**实现内容**：

1. **移动 `Sweeper` 到 `src/Controller/`**（决策 D6）：
   - `git mv lib/Sweeper/Sweeper.h lib/Sweeper/Sweeper.cpp src/Controller/`
   - 相对 include 改为 `#include "Transmitter.h"`、`#include "API.h"`（同目录，无需路径）
   - `platformio.ini` 已有 `-Isrc/Controller`；`main.cpp` 的 `#include "Sweeper.h"` 不变（同 `-I` 链）
   - 删除空目录 `lib/Sweeper/`
2. **发射接口收敛（s2）**：
   - 保留 `Transmitter::sendPocsagRaw/sendFlexRaw` 作为**正式扩展**，更新注释说明用途（Sweeper 批量/预编码路径）；invert 处理统一由这两个方法负责，Sweeper 不再自行反相
   - `sendPocsagRaw` 补前导码说明：`encodeBatch` 输出已含 preamble，调用方不重复加
   - 移除 `Sweeper::Config.txPower` 死字段（`Transmitter::Config` 无此字段，赋值无效）
3. **FLEX 批次收敛（s3，决策 D4）**：`FLEX_BATCH_SIZE = 15 → 1`；`txAddrBatch()` FLEX 分支删去 `for i < FLEX_BATCH_SIZE` 循环，改为单地址发射 + 节流（决策 D7）；header 注释删除"帧拼接"不实描述
4. **追频切频归属**：`txFreqOnce()` 的 `Si446x_setFrequencyKHz()` 调用在移入 `src/Controller` 后合规（控制层依赖驱动层），保留

**文档引用**：CLAUDE.md「项目架构」（四层）、「关键数据流」（`Transmitter::send` 统一入口）、规格 §5.2（FLEX 每批 1 地址）、§6.1（追频切频）。
**验证清单**：
- `pio run` 编译通过（重点：include 路径、无 `lib/Sweeper` 残留引用）
- `grep -r "lib/Sweeper" src/ lib/` 无残留；`git mv` 后 `git status` 显示 rename
- 串口实测 FLEX 追码：每步 1 地址，间隔符合 `intervalMs`
**反模式防护**：
- 不把 GPIO 位级操作（`digitalWrite`/`sendBytes`）移出 Transmitter（Sweeper 不得直接触碰发射引脚）
- 不删除 `send()` 标准路径（单次发射仍走 `handleTransmit → api_dispatch → send`）

---

## Phase 3：配置、状态与追频完整性（约 3 h）

**目标**：修复 a1（intervalMs）、a3（total/done）、c3（上限）、c4（频率恢复）、txPower 死字段。

**实现内容**（`src/Controller/Sweeper.h/.cpp`、`src/Controller/API.h/.cpp`、`src/Controller/WebServer.cpp`）：

1. **`Config` 扩展**：
   - 增加 `uint32_t addrEnd = 0x1FFFFF;`（决策 D1，默认 POCSAG 21-bit 上限）
   - 增加 `uint32_t freqMaxKHz = 999999;`（决策 D5）
   - 增加 `uint16_t intervalMs = 500;`（决策 D7）
   - 删除 `txPower`
   - `freqStepKHz` 保留默认 25
2. **`Status` 重构（D8）**：`{bool running; uint8_t mode; uint32_t current; uint32_t total; uint32_t done;}`，删除 `totalSent`；
   - 新增 `bool calcTotalSteps()`：addr 模式 `total = addrEnd - addrStart + 1`；freq 模式 `total = (freqMaxKHz - freqStartKHz) / freqStepKHz + 1`
   - `m_done` 每发射 1 个地址/频率 +1；`m_totalSent` 移除后，`handleSweepStop` 日志改用 `done`
3. **`loop()` 节流（D7）**：`m_nextTxMs = millis() + m_cfg.intervalMs`（批次间）；子批次间保留 10 ms 让出；FLEX 每地址后同样遵守 `intervalMs`
4. **追频恢复（D9，c4）**：`start()` 快照 `m_origFreqKHz = cfg.freqStartKHz`；`txFreqOnce()` 完成/`stop()` 时若 `m_current != m_origFreqKHz` 则 `Si446x_setFrequencyKHz(m_origFreqKHz)` 并 `api_log("SWEEP freq restored ...")`
5. **API 层**：`handleSweepStart` 填充新增字段（`addrEnd`/`freqMaxKHz`/`intervalMs` 由 `Config` 默认值承担）；`handleSweepStatus` 输出 `total`/`done`；`api_dispatch` 的 `SWEEP_START` 校验 `addrStart ≤ addrEnd`、`freqStartKHz ≤ freqMaxKHz`
6. **WebServer**：`onSweepStatus` 响应体改为 `{"code":0,"running":...,"mode":...,"current":...,"total":...,"done":...}`（规格 §9.4）

**文档引用**：规格 §3.1（Config/Status 字段）、§7.2（loop 节流）、§9.4（REST 响应）、§1.2（设计目标：不破坏现有发射）。
**验证清单**：
- `pio run` 编译通过
- 串口实测：`SWEEP START addr` 后 `SWEEP STATUS` 显示 `done/total`；追频跑完自动停止后手动 `TX` 落在**恢复后的原频率**（频谱仪/RTL-SDR 确认）
- 前端 `GET /api/sweep/status` 返回新字段
**反模式防护**：
- 不在 addr 模式写死 `total=9999999`（D1 订正）；`total` 必须与 `calcTotalSteps` 一致
- 追频恢复频率不写回 `s_config`（那是用户当前配置，恢复的是射频硬件频率，`api_getConfig` 仍返回用户值）

---

## Phase 4：命令接口与调度归属（约 2 h）

**目标**：修复 a2（SWEEP STATUS/STOP 串口命令）、a5（api_sweepStartSimple）、a6（loop 归属）、b2（STOP 别名文档化）。

**实现内容**：

1. **`SerialConsole` 扩展（a2）**：
   - `cmdSweep(args)` 支持参数覆盖（规格 §8.3 附录 A）：`SWEEP [START] addr|freq [encoding=pocsag|flex] [baud=] [phase=norm|inv] [tone=] [msgType=num|alpha] [interval=]`（`START` 前缀可选，兼容 help 与规格两种写法）
   - 新增 `cmdSweepStatus()`：`SWEEP STATUS` → 输出 `OK SWEEP running mode=addr current=... done=.../...`（规格 §8.3 示例格式）
   - `SWEEP STOP` 正式命令；裸 `STOP` 保留为别名（D10），help 文案注明
2. **`api_sweepStartSimple()`（a5）**：从 `handleSweepStart` 抽出（规格 §8.2 的 Web 快捷入口），`handleSweepStart` 内部调用它；供 `SerialConsole`/`WebServer` 复用
3. **`loop()` 归属（a6）**：`src/main.cpp` 的 `loop()` 在 `api_loop()` 之后调用 `s_sweeper.loop()`；从 `API.cpp` 的 `api_loop()` 中删除 `s_sweeper->loop()` 调用（避免双处调度语义分歧）
4. **参数校验（已实现）**：`SWEEP` 参数非法 key 或非法值（如 `baud=999`、`encoding=garbage`、`interval=abc`）返回 `ERR SWEEP: invalid parameter '<key>'`，不回 `OK`。合法值：`encoding=pocsag|flex`、`baud=512|1200|1600|2400|3200|6400`、`phase=norm|inv`、`tone=a..d`、`msgType=num|alpha`、`interval=1..60000`

**文档引用**：规格 §8.2、§8.3（含附录 A 完整语法）、§8.4（main.cpp 集成）；CLAUDE.md「数据流」。
**验证清单**：
- `pio run` 编译通过
- 串口实测全命令矩阵：`SWEEP START addr baud=512 interval=1000`、`SWEEP STATUS` 轮询、`SWEEP STOP`、`STOP` 别名
- `grep -n "s_sweeper->loop\|s_sweeper.loop" src/` 确认只在 `main.cpp` 出现
**反模式防护**：
- 不新增规格没有的命令（如 `SWEEP PAUSE`）；参数名与规格附录 A 完全一致
- 串口解析不做多余魔改（沿用 `processLine` 现有 `arg=val` 解析风格）

---

## Phase 5：前端修复（约 1.5 h）

**目标**：修复 c5（mode 显示）、对齐 D8（total/done 展示）。

**实现内容**（`data/js/config-panel.js`、`data/js/main.js`）：

1. **`updateSweepUI` mode 修复（c5）**：仅当 `running` 时按 `s.mode` 显示 `Addr`/`Freq`；`!running` 时状态行隐藏（现有 `display:none` 逻辑），不进入 else 分支显示 "Freq"
2. **进度展示（D8）**：状态行改为 `addr=<current>  <done>/<total>`（或 `freq MHz <done>/<total>`），删除 `totalSent` 引用
3. **`handleSweepStart` 响应**：`resp.start` 已由后端返回（现状），确认 terminal 文案与规格 §9.3 一致（`>>> 追码开始 addr=...`）
4. **按钮状态机**：保持规格 §9.7（扫中禁用模式按钮 + 切换按钮 STOP；D10 保留配置面板禁用，注释注明）

**文档引用**：规格 §9.3（交互流程）、§9.4（REST）、§9.7（按钮状态）。
**验证清单**：
- `data/` 上传 LittleFS（`pio run --target uploadfs`）后浏览器实测：追码/追频/停止全流程按钮状态与进度显示；IDLE 时无 "Freq" 字样
- 无 JS 报错（浏览器 console）
**反模式防护**：
- 不引入 SSE（规格 §9.5 方案 B 标注"后续增强"）；保持轮询

---

## Phase 6：TX_SETTLE_MS、文档与总验证（约 2 h）

**目标**：a4（TX_SETTLE_MS）、b1/b3 文档化、CLAUDE.md 更新、全量验证。

**实现内容**：

1. **`TX_SETTLE_MS` 10 → 2**（`src/Controller/Transmitter.cpp:13`，规格 §10.1）：
   - 依据：Si4463 PLL 锁定典型 200 µs，2 ms 含 10 倍余量
   - **实机验证前置**：改后跑一轮单次发射 + 追频，确认频谱/接收正常；若个别模块锁定慢则退回 5 ms 并记录
2. **CLAUDE.md 更新**：
   - 「项目架构」：新增 `src/Controller/Sweeper`（控制层）
   - 「关键数据流」：补充批量路径 `Sweeper::loop → txAddrBatch → Transmitter::sendPocsagRaw`（共享前导码批量，8 地址/组）
   - 「已知问题」修订：忙等待条目补充 Sweeper 批次阻塞（≤1 s POCSAG / 单帧 FLEX）；记录 D1/D2/D7/D9 决策
   - 新增小节：扫描期间 `SET_CONFIG`/`TRANSMIT` 返回 ERR_BUSY（D10）
3. **规格文档**：可选——将更新后的设计决策回写 `docs/Sweeper-设计文档.md`（若用户决定恢复该文档）或仅由 CLAUDE.md 承载
4. **全量验证**（最终门禁）：
   - `pio run` 编译零错误零警告（新增）
   - `tools/verify_pocsag_batch.py`（P1 产物）全部断言通过
   - 串口/Web 手动测试清单（下表）逐项勾选

**手动测试清单**：

| 场景 | 步骤 | 预期 |
|------|------|------|
| 追码 POCSAG | `SWEEP START addr interval=500` | 每批日志 16 地址范围；STATUS done 递增；自动停止 |
| 追码降级 | 起始地址 = 1234567（7 位） | 该地址逐地址发送，日志标注 |
| 追码 FLEX | `SWEEP START addr encoding=flex` | 每步 1 地址，间隔 500 ms |
| 追频 | `SWEEP START freq` | 每次切频 + 发射；完成后频率恢复原值 |
| 停止 | 运行中 `SWEEP STOP` / `STOP` | 立即停止，日志 `SWEEP STOP done=...` |
| 状态 | 任意时刻 `SWEEP STATUS` | `running/current/done/total` 齐全 |
| 扫描中发射 | 运行中 Web 点 TX / `TX` | 返回 ERR_BUSY（D10） |
| 恢复 | 追频完成后手动 `TX` | 落在原频率（频谱确认） |

**反模式防护**：
- 不把 `TX_SETTLE_MS` 改到 0（无余量）；实机验证不通过不合并
- 文档与实现同步：CLAUDE.md 不得描述不存在的行为

---

## 2. 执行顺序与依赖

```
P0（基线）→ P1（编码核心）→ P2（分层）→ P3（配置/状态）
        └──────────────→ P4（命令/调度）→ P5（前端）→ P6（文档+验证）
```

- P1 与 P2 有文件重叠（Sweeper.cpp 批量调用），P2 的 `git mv` 需在 P1 改完 `Sweeper` 后执行，避免合并冲突
- P3 依赖 P2（Sweeper 已入 `src/Controller`）
- P4 依赖 P3（STATUS 输出 total/done）
- P5 依赖 P3/P4（后端字段先定）
- 每阶段可独立提交；建议提交粒度：`fix(sweeper): 重写 POCSAG 批量编码帧结构`、`refactor(sweeper): 移入控制层并收敛发射接口`、`feat(sweeper): 完整状态与可配置间隔`、`feat(sweeper): 串口 SWEEP STATUS/STOP 与参数覆盖`、`fix(web): 扫描状态显示`、`chore: TX_SETTLE_MS 与文档更新`

## 3. 风险与缓解

| 风险 | 缓解 |
|------|------|
| P1 帧结构重写影响现有发射 | `genSignal()` 不动；P1 只改 `encodeBatch`，单发路径零改动；主机端脚本先验证再上板 |
| TX_SETTLE_MS 改小导致实机不稳定 | P6 实机验证门禁，回退档位 5 ms 记录 |
| 降级逻辑边界（6–7 位地址混批） | P1 编码前统一预检：批次内任一地址 >5 位则整批拆分为逐地址 |
| 前端轮询与后端字段不匹配 | P3 后端字段先行，P5 前端一次性对齐，避免双改 |
| `git mv` + include 改动编译断裂 | P2 单阶段内完成并 `pio run` 验证后再提交 |
