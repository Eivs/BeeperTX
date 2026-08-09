#pragma once

#include "API.h"

/**
 * @brief 配置持久化 — 将 TxConfig 存档到 NVS (Preferences)。
 *
 * 解决 CMD_SAVE_CONFIG 缺失问题: SET_CONFIG 只改内存配置,
 * 重启后丢失; 本类提供 load/save, 由 api_init() 开机恢复,
 * 由 CMD_SAVE_CONFIG 处理器显式落盘。
 *
 * 存储格式 (单个 blob, 带魔数 + 版本号):
 *   [magic 4B][version 2B][TxConfig 按 sizeof 原样]
 *
 * 版本号用于将来 TxConfig 布局变更时安全迁移/丢弃旧存档。
 */
class ConfigStore {
public:
  /**
   * @brief 从 NVS 读取存档。
   * @param out 输出配置
   * @return true = 存在有效存档; false = 无存档/损坏/版本不匹配
   */
  bool load(TxConfig& out);

  /**
   * @brief 将配置写入 NVS (覆盖)。
   * @return true = 写入成功
   */
  bool save(const TxConfig& cfg);

  /**
   * @brief 加载存档; 无有效存档时填入出厂默认值。
   * @param out 输出配置
   */
  void loadOrDefault(TxConfig& out);
};
