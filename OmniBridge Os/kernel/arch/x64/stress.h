#ifndef OMNIBRIDGE_STRESS_H
#define OMNIBRIDGE_STRESS_H

/*
 * 第 5 步：SLAB + 伙伴系统压力测试。
 *
 * 与 selftest.h 同构，通过编译期常量 OB_STRESS_LEVEL 选择测试级别：
 *   NONE    0  不测试（默认；等价于原来的空实现）
 *   BASIC   1  大小类遍历 + 用户 cache 往返 + 200 轮混合（原 main.c 内联版）
 *   STEADY  2  3 轮 × 4000 次随机大小交错，稳态判据（推荐日常回归）
 *   FULL    3  A/B/C/D 四段 + 稳态判据（里程碑前跑）
 *   LONG    4  FULL 的放大版：50000 次随机 + 分段（发布前跑）
 *
 * 在 main.c 中的用法：
 *     #include "stress.h"
 *     ...
 *     int rc = stress_run(OB_STRESS_LEVEL);
 *
 * 未指定 OB_STRESS_LEVEL 时默认为 NONE（不执行任何测试）。
 *
 * 判据：
 *   - BASIC  → 全程无分配失败、无 KERN_ERR。
 *   - STEADY → 第 1、2 轮结束时 free pages 相同。
 *   - FULL   → A/B/C/D 各段结束后 free pages 回到基线；
 *             随后 STEADY 判据通过。
 *   - LONG   → 50000 次随机操作的 free pages 在末两轮稳定。
 *
 * 人工审查提示：
 *   - 测试使用确定性 LCG（线性同余）产生操作序列，便于复现。
 *   - 不依赖任何 libc rand()/srand()。
 *   - 所有失败路径均做资源回收，不会给后续启动留下泄漏。
 */

#define OB_STRESS_NONE    0
#define OB_STRESS_BASIC   1
#define OB_STRESS_STEADY  2
#define OB_STRESS_FULL    3
#define OB_STRESS_LONG    4

#ifndef OB_STRESS_LEVEL
#define OB_STRESS_LEVEL OB_STRESS_NONE
#endif

/*
 * 执行压力测试。
 *   level == NONE：立即返回 0。
 *   level 其它值：执行对应级别的测试。
 *
 * 返回值：
 *   0    通过
 *  -1    失败（已通过 printk 输出诊断）
 *  -2    级别非法
 */
int stress_run(int level);

#endif /* OMNIBRIDGE_STRESS_H */