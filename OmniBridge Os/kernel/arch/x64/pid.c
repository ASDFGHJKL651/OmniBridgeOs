#include "pid.h"
#include "task.h"
#include "spinlock.h"
#include "serial.h"
#include "printk.h"

/*
 * PID 位图实现。
 *
 * 关键不变量（人工必须审查）：
 *   1) pid_init 之后，位图仅 PID 0 被置位。
 *   2) alloc_pid 返回的非负值一定满足 !pid_is_reserved()。
 *   3) alloc_pid_kernel 是唯一允许分配 1..99 的接口，仅供内核引导路径。
 *   4) free_pid 拒绝释放 0..99，防止意外清除系统保留 PID。
 *   5) 所有位操作都在 g_pid_lock 内进行。
 */

static uint8_t g_pid_bitmap[PID_BITMAP_BYTES] __attribute__((aligned(64)));
static spinlock_t g_pid_lock = SPINLOCK_INIT;
static int g_pid_inited = 0;

static inline int bit_test(const uint8_t *bm, uint64_t i)
{
    return (bm[i >> 3] >> (i & 7)) & 1;
}

static inline void bit_set(uint8_t *bm, uint64_t i)
{
    bm[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static inline void bit_clear(uint8_t *bm, uint64_t i)
{
    bm[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

void pid_init(void)
{
    if (g_pid_inited) return;

    for (uint64_t i = 0; i < PID_BITMAP_BYTES; ++i) g_pid_bitmap[i] = 0;

    /*
     * 只置位 PID 0（引导线程/空闲线程）。
     * 1..99 的"保留"语义由 pid_is_reserved() 在分配/访问路径上
     * 统一施加，无需在位图中置位。
     * 这样 alloc_pid_kernel() 才能把 1..99 分配给内核启动服务。
     */
    bit_set(g_pid_bitmap, 0);

    spin_lock_init(&g_pid_lock);
    g_pid_inited = 1;

    serial_printf("[PID] init: reserved 0..%llu, user_min=%llu, max=%llu, "
                  "bitmap=%u bytes\n",
                  (unsigned long long)PID_RESERVED_MAX,
                  (unsigned long long)PID_USER_MIN,
                  (unsigned long long)PID_MAX,
                  (unsigned)PID_BITMAP_BYTES);
}

int pid_is_reserved(uint64_t pid)
{
    return pid <= PID_RESERVED_MAX;
}

/* 在 [start, end] 内寻找第一个空闲位并置位。
 * 返回 PID，未找到返回 -1。调用者必须持有 g_pid_lock。 */
static int64_t scan_and_claim(uint64_t start, uint64_t end)
{
    if (end > PID_MAX) end = PID_MAX;
    if (start > end) return -1;
    for (uint64_t i = start; i <= end; ++i) {
        if (!bit_test(g_pid_bitmap, i)) {
            bit_set(g_pid_bitmap, i);
            return (int64_t)i;
        }
    }
    return -1;
}

int64_t alloc_pid(uint64_t requested)
{
    uint64_t flags;
    spin_lock_irqsave(&g_pid_lock, &flags);

    if (requested != 0) {
        /* 指定分配 */
        if (requested > PID_MAX) {
            spin_unlock_irqrestore(&g_pid_lock, flags);
            serial_printf("[PID] alloc: requested %llu > PID_MAX -> -EINVAL\n",
                          (unsigned long long)requested);
            return OB_EINVAL;
        }
        if (pid_is_reserved(requested)) {
            spin_unlock_irqrestore(&g_pid_lock, flags);
            serial_printf("[PID] alloc: requested %llu in reserved range "
                          "(0..%llu) -> -EINVAL\n",
                          (unsigned long long)requested,
                          (unsigned long long)PID_RESERVED_MAX);
            return OB_EINVAL;
        }
        if (bit_test(g_pid_bitmap, requested)) {
            spin_unlock_irqrestore(&g_pid_lock, flags);
            return OB_EAGAIN;
        }
        bit_set(g_pid_bitmap, requested);
        spin_unlock_irqrestore(&g_pid_lock, flags);
        return (int64_t)requested;
    }

    /* 自动分配：1000..65535 优先，回退 100..999 */
    int64_t got = scan_and_claim(PID_USER_MIN, PID_MAX);
    if (got < 0) {
        got = scan_and_claim(PID_EXT_MIN, PID_EXT_MAX);
    }
    spin_unlock_irqrestore(&g_pid_lock, flags);

    if (got < 0) return OB_EAGAIN;
    return got;
}

int free_pid(uint64_t pid)
{
    if (pid == 0) return 0;                 /* 引导 task：无 PID 可释放 */
    if (pid_is_reserved(pid)) return -1;    /* 永不释放保留 PID */

    uint64_t flags;
    spin_lock_irqsave(&g_pid_lock, &flags);
    if (!bit_test(g_pid_bitmap, pid)) {
        spin_unlock_irqrestore(&g_pid_lock, flags);
        return -1;
    }
    bit_clear(g_pid_bitmap, pid);
    spin_unlock_irqrestore(&g_pid_lock, flags);
    return 0;
}

/*
 * 内核启动服务专用：允许分配保留区 PID（1..99）。
 * 仅由 boot_services 在内核引导路径调用，用户态无法触达。
 */
int64_t alloc_pid_kernel(uint64_t requested)
{
    if (requested == 0 || requested > PID_RESERVED_MAX) {
        return OB_EINVAL;
    }

    uint64_t flags;
    spin_lock_irqsave(&g_pid_lock, &flags);

    if (bit_test(g_pid_bitmap, requested)) {
        spin_unlock_irqrestore(&g_pid_lock, flags);
        return OB_EAGAIN;
    }
    bit_set(g_pid_bitmap, requested);

    spin_unlock_irqrestore(&g_pid_lock, flags);
    return (int64_t)requested;
}