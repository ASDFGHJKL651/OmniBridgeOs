/*===OmniBridgeOs/kernel/arch/x64/oom.h===*/
#ifndef OMNIBRIDGE_OOM_H
#define OMNIBRIDGE_OOM_H

#include <stdint.h>
#include "task.h"

void oom_init(void);

/* 选择一个 OOM victim：优先杀"低权限 + 高内存占用 + 最大 PID"的进程。
 * 绝不杀 PID 1-99 或 is_critical 进程。
 * 返回 victim 指针；无候选返回 NULL。 */
struct task_t *oom_pick_victim(void);

/* ★ 修复 1：标记 victim（不立即 task_exit）。
 *   设置 victim->pending_kill = 1 与 exit_code = -9。
 *   返回 0 成功（已标记），-1 无候选。
 *   由 pmm_alloc_pages 在伙伴系统耗尽时调用。
 *
 *   人工必须审查：
 *     - 本函数不在 pmm 锁内调用 task_exit。
 *     - victim 的实际退出由 sched_tick -> oom_tick_reap() 完成。 */
int oom_mark_victim(void);

/* ★ 修复 1：由 sched_tick 每 tick 调用，处理所有 pending_kill 的 task。
 *   返回本 tick 处理的 victim 数量。
 *
 *   人工必须审查：
 *     - 绝不能在 pmm 锁内调用本函数（本函数可能 task_exit）。
 *     - victim 的 task_t 骨架由 idle 线程（task_reap_orphans）
 *       或 task_join 后续回收。 */
int oom_tick_reap(void);

#endif /* OMNIBRIDGE_OOM_H */
/*===OmniBridgeOs/kernel/arch/x64/oom.h 结束===*/