/*===OmniBridgeOs/kernel/arch/x64/futex.h===*/
/*
 * 真 futex 实现（第 18D 步任务 1）。
 *
 * 人工必须审查：
 *   - uaddr + pid 双重匹配：不同进程空间的相同虚拟地址不得互相唤醒。
 *   - futex_wait 内部不得持有任何自旋锁（设 BLOCKED 后释放锁再 schedule）。
 *   - task_exit 必须调用 futex_wake_all_for_thread 清理悬垂 waiter。
 *   - futex_wake 只唤醒，不修改用户内存；用户态 pthread_mutex_unlock
 *     先写 *lock = 0 再 syscall(FutexWake)。
 */
#ifndef OMNIBRIDGE_FUTEX_H
#define OMNIBRIDGE_FUTEX_H

#include <stdint.h>

struct thread;

#define FUTEX_MAX_BUCKETS  64
#define FUTEX_WAIT_TIMEOUT_DEFAULT 1000   /* 10 s @ 100 Hz */

#ifndef OB_ETIMEDOUT
#define OB_ETIMEDOUT (-110)
#endif
#ifndef OB_EAGAIN
#define OB_EAGAIN    (-11)
#endif

/*
 * struct futex_waiter —— 每个等待者一条记录。
 *
 * 生命周期（人工必须审查）：
 *   1. futex_wait 在栈上/由 kzalloc 分配；
 *   2. 加入 bucket[uaddr % MAX] 链表；
 *   3. schedule() 让出 CPU；
 *   4. 被 futex_wake / futex_tick / futex_wake_all_for_thread 唤醒时
 *      从链表移除，设 woken = 1，并 push 该线程回就绪队列；
 *   5. futex_wait 返回时释放（若由 kzalloc 分配）。
 *
 * 若由 futex_wake_all_for_thread 移除，woken 保持 0，futex_wait 返回
 * -EAGAIN（等待者所在线程可能已死）。
 */
struct futex_waiter {
    struct thread *th;
    uint64_t       uaddr;
    uint64_t       pid;
    uint64_t       wake_deadline;    /* 绝对 tick；0 表示无超时 */
    int            woken;
    int            _pad;
    struct futex_waiter *next;
};

void futex_init(void);

/*
 * 阻塞等待。
 *   uaddr          用户虚拟地址（4 字节，需页对齐检查由调用方完成）
 *   expected       期望值；若 *(uint32_t*)uaddr != expected，立即返回 0
 *   timeout_ticks  0 表示无限等待；否则为单位 100Hz 的 tick 数
 *
 * 返回 0 成功（被唤醒）；-ETIMEDOUT 超时；其他负错误码。
 */
int  futex_wait(uint64_t uaddr, uint32_t expected, uint64_t timeout_ticks);

/*
 * 唤醒最多 count 个等待 uaddr（且属于当前 task）的等待者。
 * count == 0 表示唤醒全部。
 * 返回实际唤醒数量。
 */
int  futex_wake(uint64_t uaddr, uint32_t count);

/* 当 th 退出时，从所有 bucket 移除属于 th 的 waiter（woken 保持 0）。 */
void futex_wake_all_for_thread(struct thread *th);

/* 时钟节拍：检查所有 bucket 的 waiter 是否超时。 */
void futex_tick(void);

/* join 路径直接唤醒指定 waiter（不修改链表结构；内部处理）。 */
void futex_wake_waiter(struct futex_waiter *w);

#endif /* OMNIBRIDGE_FUTEX_H */
/*===OmniBridgeOs/kernel/arch/x64/futex.h 结束===*/