#!/usr/bin/env bash
# 宿主机测试 — 串口 SWEEP 命令解析 (START 前缀可选 + 参数值校验) + SAVE 命令
# 编译真实 SerialConsole.cpp (stub Stream/api_dispatch), 22 用例
# 用法: bash tools/verify_serial_sweep.sh
# 退出码: 0=全部通过, 1=失败
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

cp "$REPO"/src/Controller/SerialConsole.h "$REPO"/src/Controller/SerialConsole.cpp "$DIR/"
cp "$REPO"/src/Controller/API.h "$DIR/"
cp "$REPO"/src/Controller/ProtocolTypes.h "$DIR/"
cp "$REPO"/tools/serial-test/Arduino.h "$DIR/"
cp "$REPO"/tools/serial-test/harness.cpp "$DIR/"

g++ -std=c++11 -O0 -I"$DIR" \
    "$DIR"/SerialConsole.cpp "$DIR"/harness.cpp -o "$DIR/serial_test"

"$DIR/serial_test"
