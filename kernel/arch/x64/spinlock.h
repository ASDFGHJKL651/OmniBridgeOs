#ifndef OMNIBRIDGE_SPINLOCK_H
#define OMNIBRIDGE_SPINLOCK_H

/*
 * 内核自旋锁 —— 为 SMP 做准备的最简实现。
 *
 * 实现说明（人工必须审查）：
 *   - 使用 __atomic_* 内建函数（GCC/Clang 均支持），在 freestanding 环
 *     境下不需要 libatomic。
 *   - 加锁使用 acquire 语义的 xchg；解锁使用 release 语义的 store。
 *   - 忙等期间使用 pause 指令降低功耗与内存序干扰。
 *   - irqsave/irqrestore 保存并恢复 RFLAGS.IF，保证在中断上下文与进程
 *     上下文之间不会因拿锁而自我死锁。
 *   - 本步为单核启动，锁的语义正确性在 SMP 启用后由内核双核 QEMU 压力
 *     测试验证。
 */

#include <stdint.h>

typedef struct {
    volatile int lock;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock_init(spinlock_t *l)
{
    l->lock = 0;
}

/* 获取锁（busy-wait，acquire 语义） */
static inline void spin_lock(spinlock_t *l)
{
    /* 先尝试一次快速获取 */
    if (__atomic_exchange_n(&l->lock, 1, __ATOMIC_ACQUIRE) == 0) {
        return;
    }
    /* 慢路径：自旋等待 */
    while (__atomic_load_n(&l->lock, __ATOMIC_RELAXED) != 0) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    while (__atomic_exchange_n(&l->lock, 1, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(&l->lock, __ATOMIC_RELAXED) != 0) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
}

/* 释放锁（release 语义） */
static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->lock, 0, __ATOMIC_RELEASE);
}

/* 尝试获取锁；成功返回 1，失败返回 0 */
static inline int spin_trylock(spinlock_t *l)
{
    return __atomic_exchange_n(&l->lock, 1, __ATOMIC_ACQUIRE) == 0 ? 1 : 0;
}

/* 关闭本地中断并返回 RFLAGS（包含 IF 位） */
static inline uint64_t irq_save_disable(void)
{
    uint64_t flags;
    __asm__ __volatile__(
        "pushfq\n\t"
        "popq   %0\n\t"
        "cli\n\t"
        : "=r"(flags)
        :
        : "memory", "cc");
    return flags;
}

/* 恢复之前保存的 RFLAGS（含 IF 位） */
static inline void irq_restore(uint64_t flags)
{
    __asm__ __volatile__(
        "pushq %0\n\t"
        "popfq\n\t"
        :
        : "r"(flags)
        : "memory", "cc");
}

/* 关中断 + 加锁；*flags 接收进入前的 RFLAGS */
static inline void spin_lock_irqsave(spinlock_t *l, uint64_t *flags)
{
    *flags = irq_save_disable();
    spin_lock(l);
}

/* 解锁 + 恢复 RFLAGS */
static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

#endif /* OMNIBRIDGE_SPINLOCK_H */