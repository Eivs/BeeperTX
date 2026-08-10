# FLEX SSID/NID 调研与解码器能力对比 (会话存档)

> 存档日期: 2026-08-10
> 会话结论: 当前项目 (BeeperTX) 编码端 `lib/Flex/flex.h` **不支持** SSID/NID;
> 仓库中的 `demod_flex.c` (Multimon-NG FLEX 解码器) **已完整实现** SSID/NID 解析,
> 是编码端实现的权威参考 (位布局 + 验证 oracle)。
> 本文档同时收录真机写码参数实测结论与下阶段方案。

---

## 1. 结论摘要

- **编码端 `flex.h`**: 极简单帧编码器, 仅 1600 bps / 2-FSK / 单帧 (frame=0), 无 SSID/NID。
- **解码端 `demod_flex.c`**: 5,744 行完整接收机, 支持 1600/3200/6400 bps、2/4-FSK、
  全部 BIW 类型 (SSID1/Date/Time/SysInfo/SSID2)、NID 网络向量、跨帧重组、flextime。
- **NID/SSID 实现方案**: 位布局可直接从解码器解析代码反推, 实现成本低。
- **真机实测 (Motorola Info Master)**: collapse=4 的寻呼机在 frame=0 发射下仍全部可收,
  说明该固件等效全帧扫描; 中文/英文显示差异验证了字符编码容量逻辑。
- **下阶段方向**: 优先帧参数化 + 最小帧头 (可靠性), collapse/BIW 时间可选, 跨帧 carry 按需;
  SMSG/SNUM/NNUM/HEX 不做。

---

## 2. NID/SSID 背景

FLEX 协议中两个网络标识概念 (`docs/flex-protocol-advanced-features.md` 完整版):

### 2.1 SSID (Simulcast System ID, 同播系统标识)

| 字段 | 位宽 | 说明 |
|------|------|------|
| LID (Local ID) | 9 bits (0-511) | 国家码+频段内唯一 |
| Coverage Zone | 5 bits (0-31) | 服务区域/覆盖区 |
| Country Code | 10 bits (0-1023) | ITU 国家码 |
| TMF | 4 bits | 流量管理标志 |

28-bit SSID 经两个 BIW 承载:
- **BIW000** (type 000): LID + Coverage Zone, **每帧每 phase 发**
- **BIW111** (type 111): Country Code + TMF, **仅 Frame 0-3 每 phase 发**

SSID 强制要求: ① Frame 0-3 必须传输; ② FIW 的 n 位 (漫游位) 必须 = 1;
③ GPS 时间同步 (Frame 0 对齐 GPS 整小时)。

### 2.2 NID (Network ID, 网络标识)

允许一个 System (SSID) 内划分多个逻辑网络 (多运营商共享、跨区域漫游)。
承载方式: **Network Address** + **Short Message Vector** (向量类型 010, t1t0 = 00):

| 字段 | 位宽 | 说明 |
|------|------|------|
| T3-T0 | 4 bits | 流量管理标志 |
| M2-M0 | 3 bits | 乘数 |
| A4-A0 | 5 bits | 服务区号 |

NID 出现帧: `Frame # mod 8 = (M + N + C) mod 8` (M=标称 RF 频率/25kHz 整数,
N=NID Network Address, C=Cycle 号)。一个 System 最多 8 个 NID。

---

## 3. 解码端 `demod_flex.c` 已实现的 SSID/NID 解析 (位布局参考)

### 3.1 SSID1 — BIW000 (`demod_flex.c:3540`)

```c
case 0: { // SSID1 (type 000): Local ID and Coverage Zone
  unsigned int lid = (bword >> 7) & 0x1FF;   // 9 bits
  unsigned int cov = (bword >> 16) & 0x1F;   // 5 bits
```
输出: `BIW SSID1: LID=%u CZ=%u` / JSON `biw_sysid`。

### 3.2 SSID2 — BIW111 (`demod_flex.c:3768`)

```c
case 7: { // SSID2 (type 111): Country Code and TMF
  unsigned int tmf = (bword >> 7) & 0xF;     // 4 bits
  unsigned int country = (bword >> 11) & 0x3FF; // 10 bits
```
输出: `BIW SSID2: CC=%u TMF=0x%X` / JSON `biw_countrycode`。

### 3.3 NID — Network Address + SMSG (`demod_flex.c:2742-2778`)

```c
sub_type = (phaseptr[j] >> 7) & 0x03;      // t1t0 = 00
if (flex->Decode.addr_type == 'N') {
  area_id    = (phaseptr[j] >> 9) & 0x1F;  // A0-A4, 5 bits
  multiplier = (phaseptr[j] >> 14) & 0x07; // M0-M2, 3 bits
  tmf        = (phaseptr[j] >> 17) & 0x0F; // F0-F3, 4 bits
```
地址分类 (`:1083`): Network (NID) 地址字范围 `aw 0x1F6800-0x1F77FF`,
即 capcode 2,025,472-2,029,567 (`create_short_address` = capcode + 32768)。

### 3.4 FIW n 位 (SSID 强制要求) (`demod_flex.c:1512`)

```c
flex->FIW.roaming = (fiw >> 15) & 0x1;   // 已解析并输出
```

### 3.5 编码端实现配方

| 元素 | 位置 | 内容 |
|------|------|------|
| `create_biw_ssid1(lid, cov)` | type 000 @ bits 4-6 | LID @ 7-15, CZ @ 16-20, 接 `flex_checksum` |
| `create_biw_ssid2(country, tmf)` | type 111 @ bits 4-6 | TMF @ 7-10, CC @ 11-20, 接 checksum |
| FIW n 位 | bit 15 | 置 1 (SSID 漫游要求) |
| Network Address | 短地址 capcode 2,025,472-2,029,567 | 走现有 `create_short_address` |
| NID 向量字 | type 010 @ bits 4-6, t1t0=00 | A @ 9-13, M @ 14-16, F @ 17-20 |

落地位置: `flex.h` 的 BIW 区 (`:697`, `:1158`) 追加 BIW000 + (Frame 0-3) BIW111,
新增 NID 向量 creator。验证可用 `tools/verify_flex_batch.py` 改造版闭环。

### 3.6 解码端现存缺口 (如要完善)

1. SSID 状态未聚合: LID/CZ 与 CC/TMF 逐帧分开发, 未合并成 28-bit SSID, 未存"当前系统 SSID"状态
2. NID 帧位未校验: 未查 `Frame # mod 8 = (M+N+C) mod 8`
3. SSID 变更通知未处理 (Operator Messaging Address 1110)

---

## 4. 编码/解码能力对比 (完整)

| 层次 | 能力 | demod_flex.c 解码 | flex.h 编码 |
|------|------|------------------|------------|
| PHY | 速率 | 1600/3200/6400 bps (5 种 sync code) | 仅 1600 bps |
| PHY | 调制 | 2-FSK + 4-FSK | 仅 2-FSK |
| PHY | Phase | A/B/C/D 全解 | 仅 Phase A (单帧) |
| 帧 | FIW | cycle/frame/roaming/repeat/traffic/num_tx 完整 | 硬编码 `create_fiw(0,0,0,0,0)`, n/r/t=0 |
| 帧 | BIW | BIW1 + BIW2/3/4 全 8 类型 (SSID1/Date/Time/SysInfo/SSID2) | 仅 BIW1, prio/carry/collapse=0 |
| 帧 | 多帧 | 跨帧重组 (F 序列 + R/M carry) | 单帧, ALPHA ≤248 字符 |
| 帧 | 空闲帧 | 显式处理 (voffset==aoffset) | 无 |
| 消息 | ALN | ✅ (88-3)*3 = 255 字符/帧 | ✅ (88-4)*3-4 = 248 |
| 消息 | NUM | ✅ 标准数字向量 | ✅ type 011 |
| 消息 | SMSG | ✅ t=00/01/10/11 全解 (含 NID) | ❌ 无 |
| 消息 | SNUM/NNUM | ✅ 带序号数字页 | ❌ 无 |
| 消息 | HEX/Binary | ✅ | ❌ 无 |
| 消息 | SECURE | ✅ | ❌ 无 |
| 消息 | TONE | ✅ | ✅ type 010 |
| 网络 | SSID | ✅ BIW000/BIW111 | ❌ 无 |
| 网络 | NID | ✅ Network Address + SMSG | ❌ 无 |
| 网络 | Info Service | ✅ 地址段分类 | ❌ 无 |
| 时间 | flextime | ✅ BIW Date/Time/SysInfo 投票确认 | ❌ 无 |

---

## 5. 真机实测结论 (Motorola Info Master)

### 5.1 写码参数

| 地址 | 帧编号 | 省电周期 | 消息类型 | 字符格式 |
|------|--------|----------|----------|----------|
| 37137 (E0037137) | 017 | 4 | 两者 | 中文 |
| 1234567 (1044Y1234567) | 104 | 4 | 两者 | 中文 |
| 1000 (0624Y0001000) | 062 | 4 | 两者 | 中文 |
| 1001 (0624Y0001001) | 062 | 4 | 两者 | 英文 |

消息类型可选: 个人 / 资讯 / 两者; 地址码类型: 个人 / 公共。

### 5.2 帧号公式验证 (完全一致)

`Frame = (capcode/16) % 128`, `Phase = (capcode/4) % 4`:

| capcode | Frame (写码) | Frame (公式) | Phase |
|---------|-------------|--------------|-------|
| 37137 | 017 | 17 | A |
| 1234567 | 104 | 104 | B |
| 1000 | 062 | 62 | C |
| 1001 | 062 | 62 | C |

### 5.3 关键推论: collapse=4 在 frame=0 发射下仍全收

- 理论唤醒: `Active Frame = BaseFrame + k×16` → 62/104/17/62 + k×16, **frame 0 不在其中**
- 实测全收 → 该 Info Master 固件**等效全帧扫描 (collapse=0)**, 或忽略写码 collapse 值
- 发射端 BIW1 collapse=0 广播"系统每帧在播", 与寻呼机行为共同导致可收

### 5.4 中文/英文显示 = 容量证据

- 中文: 4 行 × 10 字 = 40 字 → GB2312 双字节, 每汉字占 2 个 7-bit 字符, ≈80 字符
- 英文: 8 行 × 26 字 = 208 字 → 7-bit ASCII 每字 1 个
- 208 字符 ≈ 单帧容量上限 (248 编码上限 / 255 解码上限), 显示天花板即单帧 208

---

## 6. 下阶段方案 (基于真机实测修正)

### 6.1 决策记录

| # | 项 | 结论 | 优先级 | 理由 |
|---|----|------|--------|------|
| 1 | 帧参数可选 | ✅ 做, 默认 frame=0 | P1 | 需想清"当前帧号"来源 (GPS/RTC/手动) |
| 2 | collapse 可选 | ✅ 做, 默认 0 | P2 | 广播值非发射行为; 默认 0 最保险 |
| 3 | 空闲帧 | ⚠️ 不做完整序列, 做"最小帧头" | P1 | 单条消息无需周期广播; 最小帧头助对时/锁定 |
| 4 | BIW Date/Time | ⚠️ 需时间源: 配置/手动 + 可选联网 | P2 | 无 RTC; 单网络手动对时足够 |
| 5 | 跨帧 carry (R/M) | ✅ 可做, 优先级靠后 | P3 | 增加单条消息长度上限; 需查真机支持 |
| 6 | SMSG 数字向量 | ❌ 不做 | — | 现有 NUM 正常; 真机可能不识 SMSG, 有风险 |
| 7 | SNUM/NNUM/HEX | ❌ 不做 | — | 无需求驱动; 加类型需真机验证 |

### 6.2 推荐实施顺序

**Phase A — 可靠性提升 (P1)**
1. `flex.h` 加 `create_fiw(cycle, frame, n, r, t)` 参数化接口 (替换 `:691` 硬编码)
2. "最小帧头"发射模式: ERS 前加 FIW (带当前 frame/cycle) + BIW, ~75 字节
3. capcode→frame/phase 计算函数 (`Frame = (capcode/16)%128`)
4. 解码端验证闭环: `tools/verify_flex_batch.py` 改造, 用 `demod_flex.c` 解码生成帧核对字段

**Phase B — 锦上添花 (P2)**
5. collapse 参数暴露 (BIW1, 默认 0)
6. BIW Date/Time 广播 (时间源: 手动配置 + 可选联网校准)
   - 位布局照抄解码器: 5-bit 年份基 1994 每 32 年回绕, 6-bit 分钟, 3-bit 秒 (7.5s 分辨率)

**Phase C — 按需扩展 (P3)**
7. 跨帧 carry: FIW r 位 + BIW1 carry 位 + 消息 F 序列位三处联动
8. 视需求评估 SSID (BIW000/BIW111) 与 NID

### 6.3 明确不做

- SMSG 数字向量 (6): 现有数字寻呼机正常, 无需求驱动, 加类型有真机兼容风险
- SNUM/NNUM (7): 增量消息类型, 无实际应用场景
- HEX/Binary: **2026-08-10 真机发现后已上调为待调研 (P2)**, 见 §7.4
- 3200/6400 多速率、多 Phase: 当前硬件 (SI4463) 与单网络场景均不需要

### 6.4 风险与前提

1. **帧调度无时间源则无意义**: 帧号是时间槽, 需 GPS/RTC/手动配置提供"当前帧号"
2. **collapse 是广播值**: 改变所有寻呼机省电策略, 非单次发射行为
3. **最小帧头 vs 完整空闲帧**: 前者 ~75B 足够对时, 后者为持续发射场景设计
4. **加消息类型 = 真机验证义务**: 寻呼机不认识就是垃圾比特

---

## 7. `flex-protocol-advanced-features.md` 校验结论 (2026-08-10)

> 校验方法: 逐条对照 `demod_flex.c` 解码器实现 + `flex.h` 源码 + Info Master 真机写码参数。
> 总体结论: **文档正确性高, 与 ARIB 标准和解码器实现一致, 值得保留, 无需改写。**

### 7.1 已验证正确 (解码器证据)

| 文档声明 | 解码器证据 (demod_flex.c) |
|----------|---------------------------|
| 时间层次: 1h=15 cycle, 1 cycle=128 frame=4min, 1 frame=1.875s, 11 blocks×8 words | `:1556` `cycleno*4*60 + frameno*4*60/128`; `flex.h` `BLOCKS_PER_FRAME=11` |
| FIW 位域: cycle@4-7, frame@8-14, n@15, r@16, t@17-20 | `:1510-1514` 逐位移位一致 |
| BIW 类型表: 000=SSID1, 001=日期, 010=时间, 101=系统消息, 111=国家码+TMF, 011/100/110=保留 | `:3537-3808` switch case 0/1/2/5/7 + default "reserved" |
| BIW1 参数: prio@4-7, e_biw@8-9, s_vfield@10-15, carry@16-17, collapse@18-20 | `:3502-3510` 完全一致 |
| SSID 28 bits: LID 9b + CZ 5b + CC 10b + TMF 4b | `:3541-3542` lid@7-15, cov@16-20; `:3770-3771` tmf@7-10, cc@11-20 |
| NID 结构: A 5b + M 3b + TMF 4b, 向量 010 t=00 | `:2758-2760` area@9-13, mult@14-16, tmf@17-20 |
| collapse 公式 `BaseFrame + k×2^collapse` | `:3510` + 注释 "2^n cycle" |
| frame 公式 `(addr/16) mod 128` | 真机写码: 37137→17, 1234567→104, 1000→62, 1001→62 全部命中 |
| 能力矩阵 §9 | 与 `flex.h:691/697/713-718` 逐项一致 |

"11 种类型" 正确: 000 类型下含 BIW1/2/3/4 四个子格式, 4 + 其余 7 种 = 11。

### 7.2 4 处瑕疵

1. **§11 "frame=0 只有 1/128 概率命中" — 已被真机实测推翻** (最重要)
   - 理论分析写于 7/23, 假设寻呼机严格按 collapse 休眠
   - Info Master collapse=4 却全部收到 frame 0 发射 → 固件等效全帧扫描
   - 文档没写错, 但结论不适用于全帧扫描固件的寻呼机

2. **§8.1 Phase 公式与 §5.3 "Phase = Frame mod 4" 不完全等价**
   - `(addr/4) mod 4 ≠ ((addr/16) mod 128) mod 4` (37137: phase 0 vs frame 17 mod 4 = 1)
   - phase 公式只适用于 3200/6400 bps 多 phase 系统; 1600 bps 单 phase 无此概念

3. **§3.2 FIW 表缺 4-bit checksum**
   - c(4)+f(7)+n(1)+r(1)+t(4) = 17 bits, 21-bit 数据字段还差 4 bits (flex_checksum)

4. **§8.3 示例 "Frame = 234" 越界**
   - frame 合法范围 0-127; U-Z 类型 capcode 解析细节存疑, 非核心

### 7.3 对外部 AI 提供内容的重要警戒

**"FLEX 协议层配置" 类内容 (32-bit capcode / sub-address / 3-bit 头部 msg type /
payload length / 8-bit transparent binary mode) 是编造的**, 与 FLEX 真实结构矛盾:

| 编造声称 | FLEX 实际 |
|----------|-----------|
| Capcode 32-bit | 地址字 21-bit 数据 + BCH/奇偶 (`flex.h:165`) |
| Sub-address 2b | 无此概念; 仅 short/long 地址 (1/2 地址字, `flex.h:399`) |
| Msg Type 3-bit @ 头部 | 消息类型在 Vector Word bits 4-6, 不在头部 (`flex.h:278-326`) |
| Payload Length | 无长度字段; 由 vector msg_words + 消息结束标志隐含 |
| Binary = "8-bit transparent" | Binary/Hex 向量类型 110, 数据为 4-bit nibble 流 (`demod_flex.c:2921`) |

业务层 (BEL/ESC 控制字符、`ESC M`/`ESC G` 命令、图表坐标流) 无法证实, 疑似 AI 合理编造, 不采信。

### 7.4 Info Master 图形/股票能力 (2026-08-10 真机发现)

- 写码软件显示支持图形, 如股市信息/超级资讯, 图形开始地址 **000041C0**
- 这证明该机型固件有 FLEX Binary/HEX (`FLEX_PAGETYPE_BINARY`, 向量 110) 解析路径
- **HEX/Binary 从"明确不做"上调为"待调研" (P3→P2)**: 有真实需求信号
- 但落地瓶颈在**业务层私有格式** (图形头/股票数据表结构) 未知名;
  协议层加 Binary creator (位布局照抄 `parse_binary()`) 只是半天工作量
- 验证手段: 先用 `demod_flex.c` 验证编码端, 再真机测试
