/*===OmniBridgeOs/kernel/arch/x64/oom.c===*/
#include "oom.h"
#include "serial.h"
#include "sched.h"
#include "task.h"
#include "futex.h"      /* ★ 修复：提供 struct futex_waiter 的完整定义 */

/*
 * OOM killer（第 18D 步修复 1）。
 *
 * 设计（人工必须审查）：
 *   - 旧实现仅设置 pending_kill = 1，但该标志只被沙盒 syscall 返回路径消费。
 *     OOM 可能发生在 pmm 锁内、内核线程上下文、victim 阻塞在 hlt/futex 等
 *     场景，导致 OOM killer 事实上从不杀死任何进程。
 *   - 新实现分两阶段：
 *       (a) oom_mark_victim() —— 由 pmm_alloc_pages 在耗尽时调用，仅"标记"。
 *       (b) oom_tick_reap()   —— 由 sched_tick 每 tick 调用，真正执行回收。
 *   - 这样保证 task_exit 不在 pmm 锁内被调用（task_exit 会释放当前线程
 *     上下文并调用 schedule，持有 pmm 锁时调用会导致死锁）。
 */

void oom_init(void)
{
    serial_printf("[OOM] init: never kills PID 1-99 or critical\n");
}

struct task_t *oom_pick_victim(void)
{
    extern struct task_t *task_list_head_raw(void);

    struct task_t *best = 0;
    struct task_t *t = task_list_head_raw();

    while (t) {
        /* 跳过保留区、idle、critical、内核态沙盒 */
        if (t->pid == 0) { t = t->global_next; continue; }
        if (t->pid <= 99) { t = t->global_next; continue; }
        if (t->is_critical) { t = t->global_next; continue; }
        if (t->sandbox_flags & 0x02) { t = t->global_next; continue; }

        /* 跳过权限 9 的内核管理器 */
        if (t->privilege_level >= 9) { t = t->global_next; continue; }

        /* 跳过已标记的 victim（避免重复计数） */
        if (t->pending_kill) { t = t->global_next; continue; }

        /* 优先选择：低权限 + 高内存 + 大 PID */
        if (!best) { best = t; }
        else {
            uint64_t t_score = (uint64_t)t->mem_domain_page_count * 100 /
                               ((uint64_t)t->privilege_level + 1);
            uint64_t b_score = (uint64_t)best->mem_domain_page_count * 100 /
                               ((uint64_t)best->privilege_level + 1);
            if (t_score > b_score) best = t;
            else if (t_score == b_score && t->pid > best->pid) best = t;
        }
        t = t->global_next;
    }
    return best;
}

int oom_mark_victim(void)
{
    struct task_t *v = oom_pick_victim();
    if (!v) {
        serial_printf("[OOM] no victim found\n");
        return -1;
    }

    serial_printf("[OOM] mark pid=%llu priv=%u mem_pages=%u "
                  "(reap deferred to sched_tick)\n",
                  (unsigned long long)v->pid,
                  (unsigned)v->privilege_level,
                  (unsigned)v->mem_domain_page_count);

    /*
     * 人工必须审查：
     *   仅标记 pending_kill 与 exit_code；实际 task_exit 由 oom_tick_reap
     *   在调度器 tick 上下文执行，避免在 pmm 锁内调用 task_exit。
     */
    v->pending_kill = 1;
    v->exit_code    = -9;
    return 0;
}

int oom_tick_reap(void)
{
    struct task_t *t = task_list_head_raw();
    struct task_t *victims[8];
    int n = 0;

    /* 快照 pending_kill 的 task（不持锁） */
    while (t && n < 8) {
        if (t->pending_kill && t->pid > 99 && !t->is_critical) {
            victims[n++] = t;
        }
        t = t->global_next;
    }

    if (n == 0) return 0;

    for (int i = 0; i < n; ++i) {
        struct task_t *v = victims[i];

        serial_printf("[OOM] reap pid=%llu code=%d\n",
                      (unsigned long long)v->pid, v->exit_code);

        /* 若 victim 是当前线程，直接 task_exit */
        if (v == task_from_thread(sched_current())) {
            task_exit(v->exit_code);
            /* 不返回 */
        }

        /*
         * 人工必须审查：
         *   - 不能直接调用 task_exit(v)，因为 victim 的上下文不在本 CPU。
         *   - ★ 修复：必须调用 sched_remove_from_queue()，它会：
         *       (a) 从 ready 队列摘除 victim 的 thread（若在其中）；
         *       (b) 无条件把 state 置为 DEAD，阻断 futex/pipe 等唤醒路径
         *           通过 sched_enqueue_thread 把它重新入队。
         *   - 仅标记 state=DEAD 而无 (a)，schedule() 仍会从 rq_pop
         *     弹出并调度该线程（本轮之前日志中的 user-driver 就是如此）。
         */
        if (v->thread) {
            extern int sched_remove_from_queue(struct thread *th);
            sched_remove_from_queue(v->thread);
        }
        v->is_zombie = 1;

        /* 唤醒 join_waiter（若有） */
        struct futex_waiter *jw = v->join_waiter;
        v->join_waiter = 0;
        if (jw) {
            jw->woken = 1;
            if (jw->th) {
                extern void sched_enqueue_thread(struct thread *th);
                sched_enqueue_thread(jw->th);
            }
        }

        /* 释放用户态资源与 18D 资源（与 victim 上下文无关，可安全释放） */
        extern void user_teardown(struct task_t *t);
        extern void task_18d_cleanup(struct task_t *t);
        user_teardown(v);
        task_18d_cleanup(v);

        /* 清除 pending_kill，避免本函数被重复处理 */
        v->pending_kill = 0;
    }

    return n;
}
/*===OmniBridgeOs/kernel/arch/x64/oom.c 结束===*/