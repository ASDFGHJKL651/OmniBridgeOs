#include "task_test.h"
#include "task.h"
#include "pid.h"
#include "pmm.h"
#include "serial.h"

/*
 * 第 8 步验收测试。
 *
 * 覆盖：
 *   1) 请求保留 PID（0..99）应返回 -EINVAL（task_create 返回 NULL）。
 *   2) 权限 7 父进程的子进程权限应为 6。
 *   3) 权限 9 父进程的子进程权限应为 6。
 *   4) 权限 8 父进程的子进程权限应为 8。
 *   5) 权限 5 父进程的子进程权限应为 5（默认继承）。
 *   6) compute_is_critical 优先级（沙盒清零 > PID 1..99 > 权限 9 特权创建）。
 *   7) check_pid_access：非保留区 caller 访问保留 PID 应返回 -EPERM。
 *   8) alloc_pid / free_pid 边界。
 *
 * 生命周期（第 8 步修正 v2）：
 *   - 测试中创建的每一个 task 都通过 test_record() 记录。
 *   - 测试结束时**逆序**调用 task_destroy()：
 *       子先于父 → 避免父被销毁后再访问父的 children 链。
 *   - task_destroy 会归还 PID、内核栈（16KB）、thread、task_t。
 *
 * SLAB 缓冲预热（v2 新增，人工必须审查）：
 *   本测试涉及的 slab 大小类：
 *     - kmalloc-256  → struct task_t
 *     - kmalloc-1024 → struct thread（含 512 字节 fpu_state）
 *   这两个缓存"首次使用"时会各自从伙伴系统取 1 页建立 slab；回收后
 *   该页作为 partial 缓冲被保留（见 slab.c 的 kmem_cache_free）。这是
 *   设计意图（避免抖动），但若出现在基线采样之后，会让"页数守恒"判据
 *   出现 -2 页的假阳性。
 *
 *   修复：在采样 free_before 之前，先跑一次完整的 create/destroy 循环
 *   （"温启动"task），把这两个缓存的一次性缓冲推到基线之前。
 *   之后测试本身只应产生 0 页净变化。
 *
 *   - 本函数必须在 sched_init() 之前调用：
 *       测试 task 会被 thread_create 加入就绪队列，随后被本函数回收，
 *       其对应的 struct thread 被 kfree。这会在就绪队列中留下悬垂指针，
 *       但紧随其后的 sched_init() 只把 head/tail 置 0（不解引用），
 *       因此是安全的。若将来 sched_init() 改为遍历就绪队列，必须同步
 *       修改。
 */

/* ---------- 前向声明 ---------- */
static void test_entry_exit(void *arg);

/* ---------- 测试 task 记录 ---------- */

#define TEST_MAX_TASKS 32
static struct task_t *g_test_tasks[TEST_MAX_TASKS];
static int            g_test_task_count = 0;

static void test_record(struct task_t *t)
{
    if (!t) return;
    if (g_test_task_count >= TEST_MAX_TASKS) {
        serial_printf("[TASK] WARN: test task table full, "
                      "leak possible\n");
        return;
    }
    g_test_tasks[g_test_task_count++] = t;
}

/* 包装 task_create：自动记录，供末尾统一回收 */
static struct task_t *tcreate(const char *name,
                              struct task_t *parent,
                              uint64_t requested_pid,
                              uint8_t  requested_priv,
                              uint8_t  sandbox_flags,
                              int      privileged_creation)
{
    struct task_t *t = task_create(name, test_entry_exit, 0,
                                   parent, requested_pid,
                                   requested_priv, sandbox_flags,
                                   privileged_creation);
    if (t) test_record(t);
    return t;
}

/* dummy 入口：进入后立即退出当前 task。
 * 本函数不会在 task_selftest() 中被真正执行（调度器尚未启动），
 * 仅作为线程栈上填充的返回地址，供编译器/链接器保持符号引用。 */
static void test_entry_exit(void *arg)
{
    (void)arg;
    task_exit(0);
    for (;;) { __asm__ __volatile__("hlt"); }
}

/* 逆序回收所有记录的测试 task */
static void test_release_all(void)
{
    int n = g_test_task_count;
    serial_printf("[TASK] releasing %d test task(s)...\n", n);
    for (int i = n - 1; i >= 0; --i) {
        struct task_t *t = g_test_tasks[i];
        if (!t) continue;
        uint64_t pid = t->pid;
        task_destroy(t);
        serial_printf("[TASK] destroyed pid=%llu\n",
                      (unsigned long long)pid);
    }
    g_test_task_count = 0;
}

/* 预热 slab 缓存：用一次完整的 create/destroy 循环，把 kmalloc-256
 * 与 kmalloc-1024 的"首次使用缓冲 slab"推到基线采样之前。
 *
 * 说明：
 *   - 温启动 task 在本次函数内完成回收，不加入 g_test_tasks[]。
 *   - 内核栈在创建时从伙伴系统取 4 页、销毁时归还，净变化为 0。
 *   - 唯一遗留是 kmalloc-256 与 kmalloc-1024 各 1 页的缓冲 slab，
 *     它们会在 selftest 全程被反复复用，不构成泄漏。 */
static void warmup_slab_caches(void)
{
    struct task_t *warm = task_create("warmup", test_entry_exit, 0,
                                      0 /* parent=NULL */,
                                      0 /* pid auto */,
                                      5 /* priv */,
                                      0 /* no sandbox */,
                                      0 /* not privileged */);
    if (warm) {
        task_destroy(warm);
    }
}

static const char *priv_name(uint8_t p)
{
    static const char *names[10] = {
        "0-min", "1-vlow", "2-low", "3-elow", "4-mid",
        "5-emid", "6-high", "7-vhigh", "8-max", "9-kmgr"
    };
    return (p <= 9) ? names[p] : "?";
}

/* ---------- 入口 ---------- */

void task_selftest(void)
{
    int failures = 0;

    serial_printf("\n[TASK] === selftest begin ===\n");

    /* ---------- 预热 slab 缓存 ----------
     * 必须在采样 free_before 之前执行。 */
    warmup_slab_caches();

    /* 记录进入测试前的 PMM free 页数，用于末尾比对 */
    uint64_t free_before = pmm_free_pages_count();
    serial_printf("[TASK] free pages before: %llu\n",
                  (unsigned long long)free_before);

    /* ---------- 测试 1：请求保留 PID 应被拒绝 ---------- */
    {
        /* 注意：bad-pid50 / bad-pid1 返回 NULL，不会被记录 */
        struct task_t *bad = tcreate("bad-pid50", 0, 50, 5, 0, 0);
        if (bad == 0) {
            serial_printf("[TASK] OK  : request PID 50 -> rejected\n");
        } else {
            serial_printf("[TASK] FAIL: request PID 50 -> accepted "
                          "(pid=%llu)\n",
                          (unsigned long long)bad->pid);
            failures++;
        }

        bad = tcreate("bad-pid1", 0, 1, 8, 0, 0);
        if (bad == 0) {
            serial_printf("[TASK] OK  : request PID 1  -> rejected\n");
        } else {
            serial_printf("[TASK] FAIL: request PID 1  -> accepted\n");
            failures++;
        }
    }

    /* ---------- 测试 2：权限 7 -> 6 ---------- */
    {
        struct task_t *p7 = tcreate("p7", 0, 0, 7, 0, 0);
        if (!p7) {
            serial_printf("[TASK] FAIL: cannot create priv=7 parent\n");
            failures++;
        } else {
            struct task_t *c = tcreate("c7", p7, 0, 7, 0, 0);
            if (c && c->privilege_level == 6) {
                serial_printf("[TASK] OK  : parent %s -> child %s\n",
                              priv_name(7), priv_name(c->privilege_level));
            } else {
                serial_printf("[TASK] FAIL: parent 7 -> child %u\n",
                              c ? (unsigned)c->privilege_level : 0xFF);
                failures++;
            }
        }
    }

    /* ---------- 测试 3：权限 9 -> 6 ---------- */
    {
        struct task_t *p9 = tcreate("p9", 0, 0, 9, 0, 1 /* privileged */);
        if (!p9) {
            serial_printf("[TASK] FAIL: cannot create priv=9 parent\n");
            failures++;
        } else {
            if (p9->is_critical == 1) {
                serial_printf("[TASK] OK  : priv=9 privileged creation "
                              "is_critical=1\n");
            } else {
                serial_printf("[TASK] FAIL: priv=9 privileged creation "
                              "is_critical=%u\n",
                              (unsigned)p9->is_critical);
                failures++;
            }

            struct task_t *c = tcreate("c9", p9, 0, 9, 0, 0);
            if (c && c->privilege_level == 6) {
                serial_printf("[TASK] OK  : parent %s -> child %s\n",
                              priv_name(9), priv_name(c->privilege_level));
            } else {
                serial_printf("[TASK] FAIL: parent 9 -> child %u\n",
                              c ? (unsigned)c->privilege_level : 0xFF);
                failures++;
            }
            if (c && c->is_critical == 0) {
                serial_printf("[TASK] OK  : child of priv=9 not critical\n");
            } else if (c) {
                serial_printf("[TASK] FAIL: child of priv=9 critical=%u\n",
                              (unsigned)c->is_critical);
                failures++;
            }
        }
    }

    /* ---------- 测试 4：权限 8 -> 8 ---------- */
    {
        struct task_t *p8 = tcreate("p8", 0, 0, 8, 0, 0);
        if (!p8) {
            serial_printf("[TASK] FAIL: cannot create priv=8 parent\n");
            failures++;
        } else {
            struct task_t *c = tcreate("c8", p8, 0, 8, 0, 0);
            if (c && c->privilege_level == 8) {
                serial_printf("[TASK] OK  : parent %s -> child %s\n",
                              priv_name(8), priv_name(c->privilege_level));
            } else {
                serial_printf("[TASK] FAIL: parent 8 -> child %u\n",
                              c ? (unsigned)c->privilege_level : 0xFF);
                failures++;
            }
        }
    }

    /* ---------- 测试 5：权限 5 -> 5 ---------- */
    {
        struct task_t *p5 = tcreate("p5", 0, 0, 5, 0, 0);
        struct task_t *c  = p5 ? tcreate("c5", p5, 0, 5, 0, 0) : 0;
        if (c && c->privilege_level == 5) {
            serial_printf("[TASK] OK  : parent %s -> child %s\n",
                          priv_name(5), priv_name(c->privilege_level));
        } else {
            serial_printf("[TASK] FAIL: parent 5 -> child %u\n",
                          c ? (unsigned)c->privilege_level : 0xFF);
            failures++;
        }
    }

    /* ---------- 测试 6：is_critical 优先级 ---------- */
    {
        uint8_t r1 = compute_is_critical(50, 9, 0x01, 1);
        if (r1 == 0) {
            serial_printf("[TASK] OK  : sandbox overrides PID reserved\n");
        } else {
            serial_printf("[TASK] FAIL: sandbox override -> %u\n",
                          (unsigned)r1);
            failures++;
        }

        if (compute_is_critical(42, 0, 0, 0) == 1) {
            serial_printf("[TASK] OK  : pid=42 is_critical=1\n");
        } else {
            serial_printf("[TASK] FAIL: pid=42 is_critical!=1\n");
            failures++;
        }

        if (compute_is_critical(2000, 9, 0, 1) == 1) {
            serial_printf("[TASK] OK  : priv=9 privileged -> critical\n");
        } else {
            serial_printf("[TASK] FAIL: priv=9 privileged\n");
            failures++;
        }

        if (compute_is_critical(2001, 9, 0, 0) == 0) {
            serial_printf("[TASK] OK  : priv=9 non-privileged -> "
                          "not critical\n");
        } else {
            serial_printf("[TASK] FAIL: priv=9 non-privileged\n");
            failures++;
        }
    }

    /* ---------- 测试 7：check_pid_access ---------- */
    {
        struct task_t fake_caller;
        uint8_t *p = (uint8_t *)&fake_caller;
        for (size_t i = 0; i < sizeof(fake_caller); ++i) p[i] = 0;
        fake_caller.pid = 2000;

        int rc = check_pid_access(&fake_caller, 50);
        if (rc == OB_EPERM) {
            serial_printf("[TASK] OK  : non-reserved caller -> reserved "
                          "target returns -EPERM\n");
        } else {
            serial_printf("[TASK] FAIL: check_pid_access rc=%d\n", rc);
            failures++;
        }

        rc = check_pid_access(&fake_caller, 2000);
        if (rc == 0) {
            serial_printf("[TASK] OK  : non-reserved -> non-reserved "
                          "returns 0\n");
        } else {
            serial_printf("[TASK] FAIL: check_pid_access(non-res) rc=%d\n",
                          rc);
            failures++;
        }

        fake_caller.pid = 5;
        rc = check_pid_access(&fake_caller, 50);
        if (rc == 0) {
            serial_printf("[TASK] OK  : reserved caller -> reserved "
                          "target allowed\n");
        } else {
            serial_printf("[TASK] FAIL: reserved-reserved rc=%d\n", rc);
            failures++;
        }
    }

    /* ---------- 测试 8：alloc_pid / free_pid 边界 ---------- */
    {
        int64_t a = alloc_pid(0);
        int64_t b = alloc_pid(0);
        if (a >= (int64_t)PID_USER_MIN && b >= (int64_t)PID_USER_MIN &&
            a != b) {
            serial_printf("[TASK] OK  : auto alloc a=%lld b=%lld\n",
                          (long long)a, (long long)b);
        } else {
            serial_printf("[TASK] FAIL: auto alloc a=%lld b=%lld\n",
                          (long long)a, (long long)b);
            failures++;
        }

        int64_t r = alloc_pid(50);
        if (r == OB_EINVAL) {
            serial_printf("[TASK] OK  : alloc_pid(50) = -EINVAL\n");
        } else {
            serial_printf("[TASK] FAIL: alloc_pid(50) = %lld\n",
                          (long long)r);
            failures++;
        }

        if (a >= 0) {
            int64_t r2 = alloc_pid((uint64_t)a);
            if (r2 == OB_EAGAIN) {
                serial_printf("[TASK] OK  : alloc_pid(used) = -EAGAIN\n");
            } else {
                serial_printf("[TASK] FAIL: alloc_pid(used) = %lld\n",
                              (long long)r2);
                failures++;
            }
        }

        if (a >= 0 && free_pid((uint64_t)a) == 0) {
            serial_printf("[TASK] OK  : free_pid(%lld)\n", (long long)a);
        }
        if (b >= 0 && free_pid((uint64_t)b) == 0) {
            serial_printf("[TASK] OK  : free_pid(%lld)\n", (long long)b);
        }

        if (free_pid(50) == -1) {
            serial_printf("[TASK] OK  : free_pid(50) rejected\n");
        } else {
            serial_printf("[TASK] FAIL: free_pid(50) accepted\n");
            failures++;
        }
    }

    /* ---------- 回收所有测试 task ----------
     * 逆序销毁：子 task 先于父 task，避免父先销毁后子仍挂在父的 children 链上。
     * 完成后 PMM free_count 应恢复到 free_before。 */
    test_release_all();

    uint64_t free_after = pmm_free_pages_count();
    serial_printf("[TASK] free pages after:  %llu\n",
                  (unsigned long long)free_after);
    if (free_after == free_before) {
        serial_printf("[TASK] OK  : no page leak (%llu pages)\n",
                      (unsigned long long)free_after);
    } else {
        long long delta = (long long)free_after - (long long)free_before;
        serial_printf("[TASK] FAIL: page delta = %lld "
                      "(before=%llu after=%llu)\n",
                      delta,
                      (unsigned long long)free_before,
                      (unsigned long long)free_after);
        failures++;
    }

    if (failures == 0) {
        serial_printf("[TASK] selftest OK\n");
    } else {
        serial_printf("[TASK] selftest FAILED: %d case(s)\n", failures);
    }
    serial_printf("[TASK] === selftest end ===\n\n");
}