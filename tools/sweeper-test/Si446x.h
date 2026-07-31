// 宿主机测试 stub — Si446x 最小替代
// ⚠ 仅测试用精简接口: 勿当作生产代码修改; 若 lib/si4463/Si446x.h 变更导致编译失败,
//   应在此同步精简 (本 stub 与生产 Si446x 故意不同 — 只暴露测试所需子集)
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void Si446x_setFrequencyKHz(uint32_t freqKHz);
#ifdef __cplusplus
}
#endif
