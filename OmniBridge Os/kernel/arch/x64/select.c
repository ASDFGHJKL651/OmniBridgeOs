/*===OmniBridgeOs/kernel/arch/x64/select.c===*/
#include "select.h"
#include "vfs.h"
#include "vmm.h"
#include "serial.h"
#include "sched.h"
#include "spinlock.h"
#include "kmalloc.h"
#include "user/user.h"
#include "pmm.h"

/* ★★★ 修复 3：全局 poll 等待者链表 ★★★
 *
 * 人工必须审查：
 *   - 所有 poll 阻塞的线程加入 g_poll_waiters 单链表。
 *   - select_notify_wakeup() 由事件源（pipe/socket）状态变化时调用，
 *     遍历链表唤醒所有等待者。
 *   - 遍历时持 g_poll_wait_lock；唤醒 sched_enqueue_thread 时释放锁，
 *     避免锁序死锁。
 *   - 唤醒粒度是整个链表，不做精确筛选——18D 阶段可接受。 */
struct poll_waiter {
    struct thread *th;
    struct poll_waiter *next;
};
static struct poll_waiter *g_poll_waiters;
static spinlock_t g_poll_wait_lock = SPINLOCK_INIT;

void select_init(void)
{
    g_poll_waiters = 0;
    spin_lock_init(&g_poll_wait_lock);
    serial_printf("[SELECT] init: poll/select (event-driven)\n");
}

static short fd_ready(struct task_t *cur, int fd, short events)
{
    if (fd < 0 || fd >= 64) return POLLERR;
    if (fd <= 2) {
        return (short)(events & (POLLIN | POLLOUT));
    }
    struct vfs_file *f = cur->fd_table[fd];
    if (!f) return POLLERR;
    if (f->f_inode && f->f_inode->ops && f->f_inode->ops->poll) {
        return (short)f->f_inode->ops->poll(f, events);
    }
    return (short)(events & (POLLIN | POLLOUT));
}

static uint64_t *task_user_pml4(struct task_t *cur)
{
    if (!cur) return 0;
    struct user_ctx *uc = user_get_ctx(cur);
    if (cur->priv_iso_ready && cur->mem_domain.pml4_self_ptr) {
        return cur->mem_domain.pml4_self_ptr;
    }
    if (uc && uc->pml4) {
        return uc->pml4;
    }
    return 0;
}

static int copy_pollfd_from_user(uint64_t *pml4, uint64_t uaddr,
                                  struct pollfd *out)
{
    uint64_t va = uaddr;
    if ((va & 0xFFF) + sizeof(struct pollfd) > PAGE_SIZE) return -14;

    uint64_t *pte = vmm_get_pte(pml4, va);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -14;

    uint64_t pa = (*pte & PTE_ADDR_MASK) + (va & 0xFFF);
    const struct pollfd *src =
        (const struct pollfd *)(uintptr_t)(DIRECTMAP_BASE + pa);
    *out = *src;
    return 0;
}

static int store_pollfd_revents(uint64_t *pml4, uint64_t uaddr, short revents)
{
    uint64_t va = uaddr + 4;
    if ((va & 0xFFF) + sizeof(short) > PAGE_SIZE) return -14;

    uint64_t *pte = vmm_get_pte(pml4, va);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -14;

    uint64_t pa = (*pte & PTE_ADDR_MASK) + (va & 0xFFF);
    short *dst = (short *)(uintptr_t)(DIRECTMAP_BASE + pa);
    *dst = revents;
    return 0;
}

int64_t select_poll(struct task_t *cur, uint64_t fds_uaddr,
                    uint32_t nfds, int32_t timeout_ms)
{
    if (!cur) return -1;
    if (nfds > 64) return -22;

    struct pollfd local[64];
    if (nfds > 0) {
        if (!user_range_ok(fds_uaddr, nfds * sizeof(struct pollfd)))
            return -14;

        uint64_t *pml4 = task_user_pml4(cur);
        if (!pml4) return -1;

        for (uint32_t i = 0; i < nfds; ++i) {
            int rc = copy_pollfd_from_user(pml4,
                                           fds_uaddr + i * sizeof(struct pollfd),
                                           &local[i]);
            if (rc != 0) return rc;
        }
    }

    /* 计算超时（单位：tick，1 tick = 10ms） */
    int total_ticks = (timeout_ms < 0) ? -1 : (timeout_ms / 10);
    int elapsed = 0;

    for (;;) {
        int nready = 0;
        for (uint32_t i = 0; i < nfds; ++i) {
            short rev = fd_ready(cur, local[i].fd, local[i].events);
            local[i].revents =
                (short)(rev & (local[i].events | POLLERR | POLLHUP));
            if (local[i].revents) nready++;
        }

        if (nready > 0) {
            if (nfds > 0) {
                uint64_t *pml4 = task_user_pml4(cur);
                if (!pml4) return -1;
                for (uint32_t i = 0; i < nfds; ++i) {
                    (void)store_pollfd_revents(
                        pml4,
                        fds_uaddr + i * sizeof(struct pollfd),
                        local[i].revents);
                }
            }
            return nready;
        }

        if (total_ticks == 0) return 0;
        if (total_ticks > 0 && elapsed >= total_ticks) return 0;

        /*
         * ★★★ 修复 3：事件驱动阻塞 ★★★
         *
         * 人工必须审查：
         *   - 把当前线程加入 g_poll_waiters 链表；
         *   - 设置 state = BLOCKED；
         *   - 调用 schedule() 让出 CPU；
         *   - 被唤醒（select_notify_wakeup）后继续循环重试；
         *   - 若被唤醒时 w 仍在链表中（超时前的正常唤醒），摘除并 kfree；
         *   - 绝不能在持 g_poll_wait_lock 时调用 schedule()。
         */
        struct poll_waiter *w = (struct poll_waiter *)kzalloc(sizeof(*w));
        if (!w) {
            /* 内存不足：退化为 1 tick 忙等（避免死锁） */
            extern void thread_yield(void);
            thread_yield();
            for (volatile int j = 0; j < 100000; ++j) {
                __asm__ __volatile__("pause" ::: "memory");
            }
            elapsed++;
            continue;
        }

        struct thread *self = sched_current();
        w->th   = self;
        w->next = 0;

        uint64_t fl;
        spin_lock_irqsave(&g_poll_wait_lock, &fl);
        w->next = g_poll_waiters;
        g_poll_waiters = w;
        spin_unlock_irqrestore(&g_poll_wait_lock, fl);

        self->state = THREAD_STATE_BLOCKED;
        schedule();

        /* 被唤醒后从链表摘除（若 select_notify_wakeup 已摘除，则不会命中） */
        spin_lock_irqsave(&g_poll_wait_lock, &fl);
        struct poll_waiter **pp = &g_poll_waiters;
        while (*pp && *pp != w) pp = &(*pp)->next;
        if (*pp == w) *pp = w->next;
        spin_unlock_irqrestore(&g_poll_wait_lock, fl);
        kfree(w);

        elapsed++;
    }
}

int64_t select_select(struct task_t *cur, uint32_t nfds,
                      uint64_t rfd_uaddr, uint64_t wfd_uaddr)
{
    if (!cur) return -1;
    if (nfds == 0) return 0;
    if (nfds > 64) return -22;

    uint64_t *pml4 = task_user_pml4(cur);
    if (!pml4) return -1;

    int nready = 0;

    for (uint32_t i = 0; i < nfds; ++i) {
        short events = 0;

        if (rfd_uaddr) {
            uint64_t va = rfd_uaddr + (i >> 3);
            if (user_range_ok(va, 1)) {
                uint64_t *pte = vmm_get_pte(pml4, va);
                if (pte && (*pte & PTE_PRESENT) && (*pte & PTE_USER)) {
                    uint64_t pa = (*pte & PTE_ADDR_MASK) + (va & 0xFFF);
                    uint8_t byte =
                        *(uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
                    if (byte & (1u << (i & 7))) events |= POLLIN;
                }
            }
        }
        if (wfd_uaddr) {
            uint64_t va = wfd_uaddr + (i >> 3);
            if (user_range_ok(va, 1)) {
                uint64_t *pte = vmm_get_pte(pml4, va);
                if (pte && (*pte & PTE_PRESENT) && (*pte & PTE_USER)) {
                    uint64_t pa = (*pte & PTE_ADDR_MASK) + (va & 0xFFF);
                    uint8_t byte =
                        *(uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
                    if (byte & (1u << (i & 7))) events |= POLLOUT;
                }
            }
        }
        if (events == 0) continue;

        short rev = fd_ready(cur, (int)i, events);
        if (rev & (POLLIN | POLLOUT)) nready++;
    }

    return nready;
}

/*
 * ★★★ 修复 3：select_notify_wakeup ★★★
 *
 * 人工必须审查：
 *   - 本函数由 pipefs_read/write、net_recv 等事件源在状态变化时调用。
 *   - 遍历 g_poll_waiters 链表，唤醒所有等待者。
 *   - 唤醒时不持锁调用 sched_enqueue_thread（先摘除节点，解锁后唤醒）。
 */
void select_notify_wakeup(void)
{
    extern void sched_enqueue_thread(struct thread *th);

    /* 摘除整个链表（避免持锁唤醒） */
    uint64_t fl;
    spin_lock_irqsave(&g_poll_wait_lock, &fl);
    struct poll_waiter *w = g_poll_waiters;
    g_poll_waiters = 0;
    spin_unlock_irqrestore(&g_poll_wait_lock, fl);

    while (w) {
        struct poll_waiter *n = w->next;
        if (w->th) sched_enqueue_thread(w->th);
        kfree(w);
        w = n;
    }
}
/*===OmniBridgeOs/kernel/arch/x64/select.c 结束===*/