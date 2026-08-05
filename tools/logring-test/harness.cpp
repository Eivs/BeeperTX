// 宿主机测试 — LogRingBuffer 增量导出 (getAfter) 语义
// 用法: 由 tools/verify_logring.sh 编译运行 (复制真实 LogRingBuffer.{h,cpp})
#include "LogRingBuffer.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

static int g_pass = 0, g_fail = 0;
static void check(const char* name, bool cond) {
  if (cond) { g_pass++; printf("  PASS: %s\n", name); }
  else      { g_fail++; printf("  FAIL: %s\n", name); }
}

FakeSerialClass Serial;

// 把缓冲区导出为行数组 (便于断言)
static std::vector<std::string> toLines(char* buf) {
  std::vector<std::string> out;
  char* p = buf;
  while (*p) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    if (*p) out.push_back(p);
    if (!nl) break;
    p = nl + 1;
  }
  return out;
}

int main() {
  LogRingBuffer rb;

  // ---- 空缓冲 ----
  char buf[4096];
  rb.get(buf, sizeof(buf));
  check("空缓冲 get 返回空串", buf[0] == '\0');
  check("空缓冲 seq=0", rb.seq() == 0);

  // ---- 基本追加 + 全量导出 ----
  rb.add("line-a");
  rb.add("line-b");
  rb.add("line-c");
  check("追加后 seq=3", rb.seq() == 3);
  rb.get(buf, sizeof(buf));
  auto l3 = toLines(buf);  // 注意: toLines 就地修改 buf, 之后不可再 strcmp
  check("全量导出 3 行", l3.size() == 3 && l3[0] == "line-a" &&
        l3[1] == "line-b" && l3[2] == "line-c");

  // ---- 增量: after 语义 ----
  uint32_t seq = 0; bool wrapped = false;
  rb.getAfter(buf, sizeof(buf), 1, &seq, &wrapped);
  check("after=1 只取 seq>1", !wrapped && seq == 3 &&
        strcmp(buf, "line-b\nline-c") == 0);
  rb.getAfter(buf, sizeof(buf), 3, &seq, &wrapped);
  check("after=seq 无新行", !wrapped && seq == 3 && buf[0] == '\0');
  rb.getAfter(buf, sizeof(buf), 99, &seq, &wrapped);
  check("after>seq 无新行", !wrapped && seq == 3 && buf[0] == '\0');
  rb.getAfter(buf, sizeof(buf), 0, &seq, &wrapped);
  check("after=0 全量 + wrapped=false", !wrapped && seq == 3 &&
        strcmp(buf, "line-a\nline-b\nline-c") == 0);

  // ---- 环形覆盖 (满 50 后继续写) ----
  LogRingBuffer rb2;
  for (int i = 1; i <= 55; i++) {
    char line[32];
    snprintf(line, sizeof(line), "L%03d", i);
    rb2.add(line);
  }
  check("写满回绕后 seq=55", rb2.seq() == 55);
  // 窗口 = 最后 50 条 (seq 6..55)
  rb2.getAfter(buf, sizeof(buf), 50, &seq, &wrapped);
  check("after=50 (窗口内) 只取 51..55", !wrapped && seq == 55 &&
        strcmp(buf, "L051\nL052\nL053\nL054\nL055") == 0);
  rb2.getAfter(buf, sizeof(buf), 48, &seq, &wrapped);
  check("after=48 取 49..55 (7 行)", !wrapped && seq == 55 &&
        strcmp(buf, "L049\nL050\nL051\nL052\nL053\nL054\nL055") == 0);
  rb2.getAfter(buf, sizeof(buf), 5, &seq, &wrapped);
  // after=5 == oldest-1: 客户端最新行滚出窗口但无未送达行丢失 → 追加即可
  auto l5 = toLines(buf);
  check("after=oldest-1 无丢失 (wrapped=false 全量)", !wrapped && seq == 55 &&
        l5.size() == 50 && l5[0] == "L006" && l5.back() == "L055");
  rb2.getAfter(buf, sizeof(buf), 4, &seq, &wrapped);
  // after=4: 序号 5 已写但从未送达 (被覆盖) → 真丢失 → wrapped + 全量
  auto lw = toLines(buf);
  check("after 滚出窗口 → wrapped + 全量 50 行", wrapped && seq == 55 &&
        lw.size() == 50 && lw[0] == "L006" && lw[1] == "L007");
  rb2.getAfter(buf, sizeof(buf), 0, &seq, &wrapped);
  check("after=0 回绕后全量 50 行", !wrapped && seq == 55 &&
        toLines(buf).size() == 50);

  // ---- 兼容接口 get() 与 getAfter(0) 一致 ----
  char buf2[4096];
  rb2.getAfter(buf, sizeof(buf), 0, nullptr, nullptr);
  rb2.get(buf2, sizeof(buf2));
  check("get() == getAfter(0) 全量", strcmp(buf, buf2) == 0);

  // ---- 输出缓冲截断 ----
  char small[16];
  rb2.getAfter(small, sizeof(small), 0, nullptr, nullptr);
  check("小缓冲不越界 (含 NUL)", small[sizeof(small) - 1] == '\0');

  printf("\n结果: %d PASS, %d FAIL\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
