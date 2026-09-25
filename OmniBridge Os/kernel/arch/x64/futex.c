/*===OmniBridgeOs/kernel/arch/x64/futex.c===*/
#include "futex.h"
#include "sched.h"
#include "task.h"
#include "vmm.h"
#include "kmalloc.h"
#include "spinlock.h"
#include "serial.h"

#include "user/user.h"

static struct futex_waiter *g_buckets[FUTEX_MAX_BUCKETS];
static spinlock_t            g_bucket_lock[FUTEX_MAX_BUCKETS];
static uint64_t              g_futex_tick = 0;
static int                   g_futex_inited = 0;

static void futex_wake_one_locked(struct futex_waiter **pp, int *woken_count);

void futex_init(void)
{
    if (g_futex_inited) return;
    for (int i = 0; i < FUTEX_MAX_BUCKETS; ++i) {
        g_buckets[i] = 0;
        spin_lock_init(&g_bucket_lock[i]);
    }
    g_futex_tick = 0;
    g_futex_inited = 1;
    serial_printf("[FUTEX] init: buckets=%d\n", FUTEX_MAX_BUCKETS);
}

static uint32_t *futex_user_ptr(uint64_t uaddr)
{
    struct task_t *cur = task_from_thread(sched_current());
    if (!cur) return 0;

    struct user_ctx *uc = user_get_ctx(cur);
    uint64_t *pml4 = 0;
    if (cur->priv_iso_ready && cur->mem_domain.pml4_self_ptr) {
        pml4 = cur->mem_domain.pml4_self_ptr;
    } else if (uc && uc->pml4) {
        pml4 = uc->pml4;
    } else {
        return 0;
    }

    uint64_t *pte = vmm_get_pte(pml4, uaddr);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return 0;

    uint64_t pa = (*pte & PTE_ADDR_MASK) + (uaddr & 0xFFF);
    return (uint32_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
}

int futex_wait(uint64_t uaddr, uint32_t expected, uint64_t timeout_ticks)
{
    struct thread *self = sched_current();
    if (!self) return -1;

    struct task_t *cur = task_from_thread(self);
    if (!cur) return -1;

    uint32_t *p = futex_user_ptr(uaddr);
    if (!p) return -1;

    if (*p != expected) return 0;

    struct futex_waiter w;
    w.th            = self;
    w.uaddr         = uaddr;
    w.pid           = cur->pid;
    w.wake_deadline = timeout_ticks ? (g_futex_tick + timeout_ticks) : 0;
    w.woken         = 0;
    w.next          = 0;

    uint32_t bucket = (uint32_t)(uaddr % FUTEX_MAX_BUCKETS);

    uint64_t fl;
    spin_lock_irqsave(&g_bucket_lock[bucket], &fl);
    w.next               = g_buckets[bucket];
    g_buckets[bucket]    = &w;
    self->futex_waiter   = &w;
    spin_unlock_irqrestore(&g_bucket_lock[bucket], fl);

    self->state = THREAD_STATE_BLOCKED;
    schedule();

    int result = w.woken ? 0 : OB_ETIMEDOUT;

    spin_lock_irqsave(&g_bucket_lock[bucket], &fl);
    struct futex_waiter **pp = &g_buckets[bucket];
    while (*pp && *pp != &w) pp = &(*pp)->next;
    if (*pp == &w) *pp = w.next;
    spin_unlock_irqrestore(&g_bucket_lock[bucket], fl);

    self->futex_waiter = 0;
    return result;
}

static void futex_wake_one_locked(struct futex_waiter **pp, int *woken_count)
{
    struct futex_waiter *w = *pp;
    *pp = w->next;
    w->woken = 1;
    if (w->th) w->th->futex_waiter = 0;

    extern void sched_enqueue_thread(struct thread *th);
    if (w->th) sched_enqueue_thread(w->th);

    (*woken_count)++;
}

int futex_wake(uint64_t uaddr, uint32_t count)
{
    struct task_t *cur = task_from_thread(sched_current());
    uint64_t self_pid = cur ? cur->pid : 0;

    uint32_t bucket = (uint32_t)(uaddr % FUTEX_MAX_BUCKETS);
    int woken = 0;
    int want_all = (count == 0);

    uint64_t fl;
    spin_lock_irqsave(&g_bucket_lock[bucket], &fl);

    struct futex_waiter **pp = &g_buckets[bucket];
    while (*pp) {
        struct futex_waiter *w = *pp;
        if (w->uaddr == uaddr && w->pid == self_pid) {
            if (want_all || (uint32_t)woken < count) {
                futex_wake_one_locked(pp, &woken);
                continue;
            }
        }
        pp = &(*pp)->next;
    }

    spin_unlock_irqrestore(&g_bucket_lock[bucket], fl);
    return woken;
}

void futex_wake_all_for_thread(struct thread *th)
{
    if (!th) return;

    for (int b = 0; b < FUTEX_MAX_BUCKETS; ++b) {
        uint64_t fl;
        spin_lock_irqsave(&g_bucket_lock[b], &fl);

        struct futex_waiter **pp = &g_buckets[b];
        while (*pp) {
            if ((*pp)->th == th) {
                struct futex_waiter *w = *pp;
                *pp = w->next;
                w->woken = 0;
                th->futex_waiter = 0;
            } else {
                pp = &(*pp)->next;
            }
        }

        spin_unlock_irqrestore(&g_bucket_lock[b], fl);
    }
}

/*
 * ★★★ 修复 6：futex_tick 不再有 16-waiter 上限 ★★★
 *
 * 人工必须审查：
 *   - 旧实现使用 `struct thread *to_wake[16]` 临时数组，同一 bucket 中
 *     超过 16 个 waiter 同时超时，第 17 个及之后会被静默丢弃，导致永久阻塞。
 *   - 新实现分两阶段：
 *       (1) 在 bucket 锁内，把超时 waiter 从链表中摘除，但**不**调用
 *           sched_enqueue_thread（避免持 bucket 锁时取 sched 锁导致死锁）。
 *           把它们的 th 收集到一个动态分配的数组中。
 *       (2) 解锁后逐个唤醒，然后 kfree 数组。
 *   - 若 kzalloc 失败，退化为"逐锁-解锁"重试（保证不丢 waiter）。
 */
void futex_tick(void)
{
    g_futex_tick++;

    extern void sched_enqueue_thread(struct thread *th);

    for (int b = 0; b < FUTEX_MAX_BUCKETS; ++b) {
        uint64_t fl;
        spin_lock_irqsave(&g_bucket_lock[b], &fl);

        /* ---- 第一阶段：统计超时数量 ---- */
        int count = 0;
        for (struct futex_waiter *w = g_buckets[b]; w; w = w->next) {
            if (w->wake_deadline != 0 && g_futex_tick >= w->wake_deadline)
                count++;
        }

        if (count == 0) {
            spin_unlock_irqrestore(&g_bucket_lock[b], fl);
            continue;
        }

        /* ---- 第二阶段：分配收集数组 ---- */
        struct thread **batch = (struct thread **)kzalloc(
            (size_t)count * sizeof(struct thread *));
        if (!batch) {
            /*
             * 人工必须审查：kzalloc 失败时退化为"边遍历边唤醒"。
             * 为避免持锁调用 sched_enqueue_thread，采用"解锁→唤醒→重锁"
             * 折衷：每处理一个 waiter 就解锁重锁。
             */
            struct futex_waiter **pp = &g_buckets[b];
            while (*pp) {
                struct futex_waiter *w = *pp;
                if (w->wake_deadline != 0 &&
                    g_futex_tick >= w->wake_deadline) {
                    *pp = w->next;
                    w->woken = 0;
                    if (w->th) w->th->futex_waiter = 0;
                    struct thread *th = w->th;

                    spin_unlock_irqrestore(&g_bucket_lock[b], fl);
                    if (th) sched_enqueue_thread(th);
                    spin_lock_irqsave(&g_bucket_lock[b], &fl);

                    /* 重锁后从头扫描（链表可能已变） */
                    pp = &g_buckets[b];
                    while (*pp && (*pp)->th != th) pp = &(*pp)->next;
                    continue;
                }
                pp = &(*pp)->next;
            }
            spin_unlock_irqrestore(&g_bucket_lock[b], fl);
            continue;
        }

        /* ---- 第三阶段：摘除链表节点，收集 th ---- */
        int n = 0;
        struct futex_waiter **pp = &g_buckets[b];
        while (*pp && n < count) {
            struct futex_waiter *w = *pp;
            if (w->wake_deadline != 0 && g_futex_tick >= w->wake_deadline) {
                *pp = w->next;
                w->woken = 0;
                if (w->th) w->th->futex_waiter = 0;
                batch[n++] = w->th;
                continue;
            }
            pp = &(*pp)->next;
        }

        spin_unlock_irqrestore(&g_bucket_lock[b], fl);

        /* ---- 第四阶段：锁外唤醒 ---- */
        for (int i = 0; i < n; ++i) {
            if (batch[i]) sched_enqueue_thread(batch[i]);
        }
        kfree(batch);
    }
}

void futex_wake_waiter(struct futex_waiter *w)
{
    if (!w) return;

    uint32_t bucket = (uint32_t)(w->uaddr % FUTEX_MAX_BUCKETS);
    uint64_t fl;
    spin_lock_irqsave(&g_bucket_lock[bucket], &fl);

    struct futex_waiter **pp = &g_buckets[bucket];
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp == w) {
        *pp = w->next;
        w->woken = 1;
        if (w->th) w->th->futex_waiter = 0;
    }

    spin_unlock_irqrestore(&g_bucket_lock[bucket], fl);

    extern void sched_enqueue_thread(struct thread *th);
    if (w->th) sched_enqueue_thread(w->th);
}
/*===OmniBridgeOs/kernel/arch/x64/futex.c 结束===*/