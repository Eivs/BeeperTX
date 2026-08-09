#!/usr/bin/env bash
# 宿主机状态机测试 — 编译真实 Sweeper.cpp + 真实 Pocsag.cpp (stub 外部依赖)
# 用法: bash tools/verify_sweeper_state.sh
# 退出码: 0=全部通过, 1=失败
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

# 复制被测真实源码 + stub 头 + harness
cp "$REPO"/src/Controller/Sweeper.h "$REPO"/src/Controller/Sweeper.cpp "$DIR/"
cp "$REPO"/src/Controller/ProtocolTypes.h "$DIR/"
cp "$REPO"/lib/Pocsag/Pocsag.h "$DIR/"
cp "$REPO"/tools/sweeper-test/*.h "$DIR/"
cp "$REPO"/tools/sweeper-test/harness.cpp "$DIR/"

g++ -std=c++11 -O0 -I"$DIR" -I"$REPO/lib/Pocsag" \
    "$DIR"/Sweeper.cpp "$REPO"/lib/Pocsag/Pocsag.cpp \
    "$DIR"/harness.cpp -o "$DIR/sweeper_test"

"$DIR/sweeper_test"
