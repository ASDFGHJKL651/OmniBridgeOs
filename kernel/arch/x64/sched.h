#ifndef OMNIBRIDGE_SCHED_H
#define OMNIBRIDGE_SCHED_H

#include <stdint.h>
#include <stddef.h>

/*
 * 多级反馈队列 + 严格优先级混合调度器 v0.2（第 8 步：加入 task 反向指针）。
 *
 * 第 8 步改动：
 *   - struct thread 新增 void *task 字段，用于向上还原 task_t。
 *   - sched.c 不需改动头文件依赖：调度器仍然只认识 struct thread。
 *   - task_t 与 struct thread 的转换通过 task.h 提供的
 *     task_from_thread() / thread_from_task() 两个内联函数完成。
 */

#define THREAD_STATE_READY    0
#define THREAD_STATE_RUNNING  1
#define THREAD_STATE_BLOCKED  2
#define THREAD_STATE_DEAD     3

#define SCHED_NUM_QUEUES      3

#define SCHED_Q_LOW           0
#define SCHED_Q_MID           1
#define SCHED_Q_HIGH          2

/* 时间片（单位：tick，100Hz 下 1 tick = 10ms） */
#define SCHED_SLICE_LOW_TICKS   2
#define SCHED_SLICE_MID_TICKS   5
#define SCHED_SLICE_HIGH_TICKS  8

/* 内核线程栈大小（字节）：16KB = 4 个 4KB 页，order = 2 */
#define THREAD_KSTACK_SIZE      (16 * 1024)

struct thread {
    /* ★ 必须是第一个字段，context.S 使用偏移 0 保存/恢复 RSP */
    uint64_t rsp;

    uint64_t tid;
    int      state;
    int      priority;
    int      queue;
    uint64_t slice_remaining;
    uint64_t total_ticks;

    struct thread *next;        /* 就绪队列链 */
    struct thread *prev;

    const char *name;

    /* 线程入口函数与参数（thread_start_trampoline 使用 r14/r15 读取） */
    void (*entry)(void *);
    void  *arg;

    /* ★ 第 8 步：反向指针，指向拥有此线程的 task_t。 */
    void  *task;

    /* ★ 第 18D 步：futex 等待信息。
     *   - 非 NULL 时表示本线程正在某个 futex bucket 中等待；
     *   - 由 futex_wait 设置，由 futex_wake / futex_tick /
     *     futex_wake_all_for_thread 清除。 */
    struct futex_waiter *futex_waiter;

    /* 内核栈基址（用于调试 / 将来释放） */
    void   *kstack_base;
    size_t  kstack_size;
    int     kstack_order;

    /* 全局线程链，仅用于调试 */
    struct thread *g_next;

    /* FPU 保存区 */
    uint8_t fpu_state[512] __attribute__((aligned(16)));

    /* ★ 修复 OOM reap：标记本 thread 当前是否位于某个就绪队列中。
     *   - 由 rq_push/rq_pop/rq_remove 统一维护；
     *   - 用于安全判断"能否从 ready 队列摘除"，避免 rq_remove 误清空
     *     队首/队尾指针；
     *   - 也用于 sched_enqueue_thread 防止重复入队。 */
    uint8_t in_rq;
    uint8_t _pad_rq[3];

} __attribute__((aligned(16)));

/* ★ 第 18D 步：调度器内部函数，供 futex.c 唤醒线程使用。
 *
 * 语义：把线程重新加入就绪队列尾部，状态设为 READY。
 * 调用者不得持有 g_sched_lock（本函数内部获取）。
 */
void sched_enqueue_thread(struct thread *th);

/* 初始化调度器（清空队列、初始化引导线程占位符） */
void sched_init(void);

/*
 * 创建内核线程并加入就绪队列。
 *   priority: 0..9，映射到 3 个队列
 * 返回新线程指针；失败返回 NULL。
 */
struct thread *thread_create(const char *name,
                             void (*entry)(void *), void *arg,
                             int priority);

/* 时钟 tick 处理：递减当前线程时间片，必要时触发调度 */
void sched_tick(void);

/* 调度：选择下一个就绪线程并切换；可从上下文调用 */
void schedule(void);

/* 主动让出 CPU：等价于把当前线程放到相同队列尾部 */
void thread_yield(void);

/* 终止当前线程（不返回） */
void thread_exit(void);

/* 当前线程指针 */
struct thread *sched_current(void);

/* 启动调度器：从引导上下文切入第一个就绪线程，不返回 */
void sched_start(void);

/* ★ 修复 OOM reap：把 th 从就绪队列摘除并标记 DEAD。
 *
 * 语义：
 *   - 若 th 当前位于某个 ready 队列中，摘除它；
 *   - 无论是否在 ready 队列中，都把 th->state 置为 THREAD_STATE_DEAD，
 *     使后续 sched_enqueue_thread 拒绝把它重新入队；
 *   - 不释放 th 本身或对应的 task_t 骨架，由 task_destroy / task_reap_orphans
 *     负责回收。
 *
 * 调用者不得持有 g_sched_lock（本函数内部获取）。
 * 返回 0 成功；-1 参数无效。 */
int sched_remove_from_queue(struct thread *th);

#endif /* OMNIBRIDGE_SCHED_H */