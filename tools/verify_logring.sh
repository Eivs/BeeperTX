#!/usr/bin/env bash
# 宿主机测试 — LogRingBuffer 增量导出 (getAfter) 语义
# 编译真实 LogRingBuffer.cpp (stub 外部依赖)
# 用法: bash tools/verify_logring.sh
# 退出码: 0=全部通过, 1=失败
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

# 复制被测真实源码 + stub 头 + harness
cp "$REPO"/src/Controller/LogRingBuffer.h "$REPO"/src/Controller/LogRingBuffer.cpp "$DIR/"
cp "$REPO"/tools/logring-test/Arduino.h "$DIR/"
cp "$REPO"/tools/logring-test/harness.cpp "$DIR/"

g++ -std=c++11 -O0 -I"$DIR" \
    "$DIR"/LogRingBuffer.cpp "$DIR"/harness.cpp -o "$DIR/logring_test"

"$DIR/logring_test"
