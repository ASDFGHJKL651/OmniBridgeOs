/*===OmniBridgeOs/kernel/arch/x64/sched.c===*/
#include "sched.h"
#include "kmalloc.h"
#include "pmm.h"
#include "printk.h"
#include "serial.h"
#include "spinlock.h"
#include "task.h"
#include "vmm.h"
#include "user/user.h"

extern void context_switch(struct thread *prev, struct thread *next);
extern void thread_start_trampoline(void);
extern uint64_t g_syscall_kernel_rsp;
static int rq_remove(int q, struct thread *t);

extern void net_tick(void);

static struct thread *g_rq_head[SCHED_NUM_QUEUES];
static struct thread *g_rq_tail[SCHED_NUM_QUEUES];

static struct thread *g_current = 0;
static struct thread *g_idle    = 0;
static struct thread *g_all     = 0;

static spinlock_t g_sched_lock = SPINLOCK_INIT;

static uint64_t g_next_tid = 1;

static struct thread  g_bootstrap;
static int            g_bootstrap_used = 0;

static int prio_to_queue(int prio)
{
    if (prio < 3) return SCHED_Q_LOW;
    if (prio < 6) return SCHED_Q_MID;
    return SCHED_Q_HIGH;
}

static uint64_t queue_slice(int q)
{
    switch (q) {
    case SCHED_Q_LOW:  return SCHED_SLICE_LOW_TICKS;
    case SCHED_Q_MID:  return SCHED_SLICE_MID_TICKS;
    case SCHED_Q_HIGH: return SCHED_SLICE_HIGH_TICKS;
    default:           return SCHED_SLICE_LOW_TICKS;
    }
}

static int size_to_order(size_t size)
{
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while (order < MAX_ORDER && ((size_t)1 << order) < pages) {
        order++;
    }
    return order;
}

static void rq_push(int q, struct thread *t)
{
    /* ★ 修复：若 thread 已在某个 ready 队列中，拒绝重复入队。
     *   原来靠调用点自行判断（不可靠），现在由本函数自身保证。 */
    if (t->in_rq) {
        serial_printf("[SCHED] WARN: rq_push tid=%llu already in rq, drop\n",
                      (unsigned long long)t->tid);
        return;
    }

    t->next  = 0;
    t->prev  = g_rq_tail[q];
    t->queue = q;
    t->in_rq = 1;                 /* ★ */
    if (g_rq_tail[q]) {
        g_rq_tail[q]->next = t;
    } else {
        g_rq_head[q] = t;
    }
    g_rq_tail[q] = t;
}

static struct thread *rq_pop(int q)
{
    struct thread *t = g_rq_head[q];
    if (!t) return 0;
    g_rq_head[q] = t->next;
    if (g_rq_head[q]) {
        g_rq_head[q]->prev = 0;
    } else {
        g_rq_tail[q] = 0;
    }
    t->next = 0;
    t->prev = 0;
    t->in_rq = 0;                /* ★ */
    return t;
}

static int rq_remove(int q, struct thread *t)
{
    if (!t) return -1;

    /* ★ 修复：只有 in_rq==1 的线程才真正在队列里。
     *   否则（正在运行 / 阻塞 / 已 DEAD）直接返回，避免误把
     *   g_rq_head[q] 或 g_rq_tail[q] 清成 0。 */
    if (!t->in_rq) return -1;

    if (t->prev) t->prev->next = t->next;
    else         g_rq_head[q]  = t->next;
    if (t->next) t->next->prev = t->prev;
    else         g_rq_tail[q]  = t->prev;
    t->next  = 0;
    t->prev  = 0;
    t->in_rq = 0;                /* ★ */
    return 0;
}

void sched_init(void)
{
    for (int i = 0; i < SCHED_NUM_QUEUES; ++i) {
        g_rq_head[i] = 0;
        g_rq_tail[i] = 0;
    }
    g_current = 0;
    g_idle    = 0;
    g_all     = 0;
    g_next_tid = 1;
    spin_lock_init(&g_sched_lock);

    if (!g_bootstrap_used) {
        uint8_t *p = (uint8_t *)&g_bootstrap;
        for (size_t i = 0; i < sizeof(g_bootstrap); ++i) p[i] = 0;
        g_bootstrap.tid             = 0;
        g_bootstrap.state           = THREAD_STATE_DEAD;
        g_bootstrap.priority        = 5;
        g_bootstrap.queue           = SCHED_Q_MID;
        g_bootstrap.slice_remaining = 0;
        g_bootstrap.name            = "bootstrap";
        g_bootstrap.task            = 0;
        g_bootstrap_used            = 1;
    }
    g_current = &g_bootstrap;

    serial_printf("[SCHED] init: %d queues, slices=%u/%u/%u ticks\n",
                  SCHED_NUM_QUEUES,
                  (unsigned)SCHED_SLICE_LOW_TICKS,
                  (unsigned)SCHED_SLICE_MID_TICKS,
                  (unsigned)SCHED_SLICE_HIGH_TICKS);
}

struct thread *thread_create(const char *name,
                             void (*entry)(void *), void *arg,
                             int priority)
{
    if (!entry) return 0;

    struct thread *t = (struct thread *)kzalloc(sizeof(struct thread));
    if (!t) {
        printk(KERN_ERR "[SCHED] thread_create: PCB alloc failed\n");
        return 0;
    }

    int order = size_to_order(THREAD_KSTACK_SIZE);
    if (order >= MAX_ORDER) {
        kfree(t);
        printk(KERN_ERR "[SCHED] thread_create: kstack size too large\n");
        return 0;
    }

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) {
        kfree(t);
        printk(KERN_ERR
               "[SCHED] thread_create: kstack alloc failed (order=%d)\n",
               order);
        return 0;
    }

    void *stack = (void *)(uintptr_t)(DIRECTMAP_BASE + page_to_phys(pg));

    t->state     = THREAD_STATE_READY;
    t->priority  = (priority < 0) ? 0 : (priority > 9 ? 9 : priority);
    t->queue     = prio_to_queue(t->priority);
    t->slice_remaining = queue_slice(t->queue);
    t->total_ticks     = 0;
    t->name      = name;
    t->entry     = entry;
    t->arg       = arg;
    t->task      = 0;
    t->kstack_base  = stack;
    t->kstack_size  = (size_t)1 << (order + PAGE_SHIFT);
    t->kstack_order = order;
    t->next = 0;
    t->prev = 0;
    t->g_next = 0;
    t->futex_waiter = 0;

    uintptr_t top = ((uintptr_t)stack + t->kstack_size) & ~(uintptr_t)0xF;
    uint64_t *sp = (uint64_t *)(top - 7 * sizeof(uint64_t));

    sp[0] = (uint64_t)(uintptr_t)entry;
    sp[1] = (uint64_t)(uintptr_t)arg;
    sp[2] = 0;
    sp[3] = 0;
    sp[4] = 0;
    sp[5] = 0;
    sp[6] = (uint64_t)(uintptr_t)thread_start_trampoline;

    t->rsp = (uint64_t)(uintptr_t)sp;

    uint64_t flags;
    spin_lock_irqsave(&g_sched_lock, &flags);
    t->tid    = g_next_tid++;
    t->g_next = g_all;
    g_all     = t;
    rq_push(t->queue, t);
    spin_unlock_irqrestore(&g_sched_lock, flags);

    serial_printf("[SCHED] thread '%s' created tid=%llu prio=%d q=%d "
                  "kstack=0x%llx order=%d\n",
                  name ? name : "(null)",
                  (unsigned long long)t->tid,
                  t->priority, t->queue,
                  (unsigned long long)(uintptr_t)t->kstack_base,
                  t->kstack_order);
    return t;
}

void schedule(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_sched_lock, &flags);

    struct thread *prev = g_current;
    struct thread *next = 0;

    if (prev && prev != g_idle && prev->state == THREAD_STATE_RUNNING) {
        if (prev->slice_remaining > 0) {
            int need_preempt = 0;
            for (int q = prev->queue + 1; q < SCHED_NUM_QUEUES; ++q) {
                if (g_rq_head[q]) { need_preempt = 1; break; }
            }
            if (!need_preempt) {
                spin_unlock_irqrestore(&g_sched_lock, flags);
                return;
            }
        }
        prev->state = THREAD_STATE_READY;
        prev->slice_remaining = queue_slice(prev->queue);
        rq_push(prev->queue, prev);
    }

    for (int q = SCHED_NUM_QUEUES - 1; q >= 0; --q) {
        while ((next = rq_pop(q)) != 0) {
            if (next->state != THREAD_STATE_DEAD) break;
            /* 防御：理论上 rq_remove 已摘除 DEAD 线程，
             *   这里兜底丢弃，避免运行时状态异常导致跳到死线程。 */
            serial_printf("[SCHED] drop DEAD thread tid=%llu from q=%d\n",
                          (unsigned long long)next->tid, q);
            next = 0;
        }
        if (next) break;
    }
    if (!next) next = g_idle;

    if (prev == next) {
        if (next) next->state = THREAD_STATE_RUNNING;
        spin_unlock_irqrestore(&g_sched_lock, flags);
        return;
    }

    g_current = next;
    if (next) next->state = THREAD_STATE_RUNNING;

    spin_unlock(&g_sched_lock);

    if (next) {
        struct task_t *next_task = task_from_thread(next);
        uint64_t *next_pml4 = 0;

        if (next_task) {
            if (next_task->priv_iso_ready &&
                next_task->mem_domain.pml4_self_ptr) {
                next_pml4 = next_task->mem_domain.pml4_self_ptr;
            } else {
                struct user_ctx *uc = user_get_ctx(next_task);
                if (uc && uc->pml4) {
                    next_pml4 = uc->pml4;
                }
            }
        }

        if (next_pml4) {
            vmm_switch_address_space(next_pml4);
        } else {
            uint64_t *kpml4 = vmm_kernel_pml4();
            if (kpml4) vmm_switch_address_space(kpml4);
        }
    }

    if (next) {
        uint64_t ktop = (uint64_t)next->kstack_base + next->kstack_size;
        g_syscall_kernel_rsp = ktop & ~0xFULL;
    }

    context_switch(prev, next);

    irq_restore(flags);
}

uint64_t sched_get_tick(void) { return g_current ? g_current->total_ticks : 0; }

void sched_tick(void)
{
    extern void futex_tick(void);
    extern void net_tick(void);
    futex_tick();

    /* ★★★ 修复 1：OOM tick reap ★★★
     *
     * 人工必须审查：
     *   - 该调用必须在 futex_tick 之后，且在 schedule() 之前执行。
     *   - oom_tick_reap 内部可能对 victim 调用 task_exit（当 victim 就是
     *     当前线程时）。这在 sched_tick 上下文中是安全的，因为
     *     sched_tick 不持有任何 pmm 锁或调度器锁。
     *   - 如果 victim 是别的线程，仅标记 DEAD + zombie，唤醒其
     *     join_waiter，释放用户态资源。 */
    extern int oom_tick_reap(void);
    oom_tick_reap();

    if (!g_current) return;

    uint64_t need_resched = 0;

    if (g_current == g_idle) {
        uint64_t flags;
        spin_lock_irqsave(&g_sched_lock, &flags);
        for (int q = 0; q < SCHED_NUM_QUEUES; ++q) {
            if (g_rq_head[q]) { need_resched = 1; break; }
        }
        spin_unlock_irqrestore(&g_sched_lock, flags);

        net_tick();

        if (need_resched) schedule();
        return;
    }

    uint64_t flags;
    spin_lock_irqsave(&g_sched_lock, &flags);
    if (g_current->slice_remaining > 0) g_current->slice_remaining--;
    g_current->total_ticks++;

    /* ★★★ 修复 5：CPU 配额降级/恢复条件修正 ★★★
     *
     * 人工必须审查：
     *   - 旧实现 `used == 0 && queue == LOW` 几乎永不成立
     *     （total_ticks 刚递增后 used = total_ticks % 100 至少为 1），
     *     导致被降级的线程永久留在 LOW 队列。
     *   - 新实现用 `used == 1` 作为"刚进入新窗口"的边界条件。
     *   - 若线程在窗口中途创建，total_ticks 从 0 开始，used 从 1 开始，
     *     第一次就满足 used == 1；此时若 queue 不是 LOW，
     *     不进入本分支（安全）。 */
    {
        struct task_t *tk = task_from_thread(g_current);
        if (tk && tk->cpu_usage_quota > 0 && tk->cpu_usage_quota < 100) {
            uint64_t used = g_current->total_ticks % 100;
            if (used > tk->cpu_usage_quota &&
                g_current->queue != SCHED_Q_LOW) {
                rq_remove(g_current->queue, g_current);
                g_current->queue = SCHED_Q_LOW;
                rq_push(SCHED_Q_LOW, g_current);
                serial_printf("[SCHED] quota demote tid=%llu to LOW\n",
                              (unsigned long long)g_current->tid);
            } else if (used == 1 && g_current->queue == SCHED_Q_LOW) {
                g_current->queue = prio_to_queue(g_current->priority);
                rq_remove(SCHED_Q_LOW, g_current);
                rq_push(g_current->queue, g_current);
                serial_printf("[SCHED] quota restore tid=%llu to q=%d\n",
                              (unsigned long long)g_current->tid,
                              g_current->queue);
            }
        }
    }

    if (g_current->slice_remaining == 0) need_resched = 1;
    spin_unlock_irqrestore(&g_sched_lock, flags);

    net_tick();

    if (need_resched) schedule();
}

void thread_yield(void)
{
    if (g_current) g_current->slice_remaining = 0;
    schedule();
}

void thread_exit(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_sched_lock, &flags);
    if (g_current && g_current != g_idle) {
        g_current->state = THREAD_STATE_DEAD;
    }
    spin_unlock_irqrestore(&g_sched_lock, flags);

    schedule();
    for (;;) { __asm__ __volatile__("hlt"); }
}

struct thread *sched_current(void)
{
    return g_current;
}

/* ★ 修复 OOM reap：从 ready 队列摘除并标记 DEAD。
 *   调用后该 thread 不会再被 schedule() 挑中；也不在 ready 队列里，
 *   所以 rq_remove 对它再次调用无副作用。
 *   由 task_destroy / task_reap_orphans 负责最终释放 thread 结构。 */
int sched_remove_from_queue(struct thread *th)
{
    if (!th) return -1;

    uint64_t flags;
    spin_lock_irqsave(&g_sched_lock, &flags);

    /* 若在 ready 队列中，摘除 */
    (void)rq_remove(th->queue, th);

    /* 无论之前在何种状态，都标记 DEAD：
     *   - READY 队列中 → 已摘除，不再被调度；
     *   - RUNNING（即调用者，不会走到这里）→ 交由 task_exit；
     *   - BLOCKED（如 futex/pipe）→ 阻断后续 sched_enqueue_thread；
     *   - 已经是 DEAD → 幂等。 */
    th->state = THREAD_STATE_DEAD;

    spin_unlock_irqrestore(&g_sched_lock, flags);
    return 0;
}

void sched_enqueue_thread(struct thread *th)
{
    if (!th) return;
    if (th->state == THREAD_STATE_DEAD) return;

    uint64_t flags;
    spin_lock_irqsave(&g_sched_lock, &flags);

    /* ★ 修复：用 in_rq 精确判断，替换原先不可靠的
     *   `state==READY && prev!=0`（对队首线程失效，可能重复入队）。 */
    if (!th->in_rq) {
        th->state = THREAD_STATE_READY;
        th->slice_remaining = queue_slice(th->queue);
        rq_push(th->queue, th);
    }

    spin_unlock_irqrestore(&g_sched_lock, flags);
}

void sched_start(void)
{
    serial_printf("[SCHED] starting...\n");
    schedule();

    printk(KERN_ERR "[SCHED] sched_start returned unexpectedly\n");
    for (;;) { __asm__ __volatile__("hlt"); }
}
/*===OmniBridgeOs/kernel/arch/x64/sched.c 结束===*/