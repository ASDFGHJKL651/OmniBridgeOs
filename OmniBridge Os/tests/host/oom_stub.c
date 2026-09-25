/*===OmniBridgeOs/tests/host/oom_stub.c===*/
/*
 * host 侧 oom_mark_victim 桩。
 *
 * 背景（人工必须审查）：
 *   pmm.c 在伙伴系统耗尽时调用 oom_mark_victim()（第 18D 步新增）。
 *   宿主测试不实例化 OOM killer（不链接 sched.c / task.c / futex.c），
 *   因此必须提供此桩，否则链接报 undefined reference。
 */
#include <stdint.h>

int oom_mark_victim(void) { return -1; }