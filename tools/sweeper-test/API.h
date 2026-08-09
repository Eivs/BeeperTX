// 宿主机测试 stub — API 最小替代 (仅 api_log)
// ⚠ 仅测试用精简接口: 勿当作生产代码修改; 若生产 API.h 变更导致编译失败,
//   应在此同步精简 (本 stub 与 src/Controller/API.h 故意不同 — 只暴露测试所需子集)
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void api_log(const char* msg);
#ifdef __cplusplus
}
#endif
