/*===OmniBridgeOs/kernel/arch/x64/task.c===*/
#include "task.h"
#include "pid.h"
#include "sched.h"
#include "kmalloc.h"
#include "pmm.h"
#include "printk.h"
#include "serial.h"
#include "spinlock.h"
#include "audit.h"
#include "vmm.h"
#include "mem_domain.h"
#include "priv_iso.h"
#include "compat_preload.h"
#include "compat_handle.h"
#include "net/netns.h"
#include "user/user.h"
#include "futex.h"
#include "vfs.h"

struct futex_waiter;

static struct task_t *g_task_head = 0;
static spinlock_t     g_task_lock = SPINLOCK_INIT;
static uint64_t       g_task_count = 0;

static struct task_t  g_bootstrap_task_storage;

/* ---- 18D：zombie 语义 ---- */

void task_18d_init(struct task_t *t)
{
    if (!t) return;

    t->is_zombie     = 0;
    t->join_waiter   = 0;
    t->shm_list      = 0;
    t->seccomp_mode  = 0;
    t->seccomp_filter = 0;
    t->pidns         = 0;
    t->mntns         = 0;
    t->ipcns         = 0;
    t->ptrace_flags  = 0;
    t->ptrace_tracer_pid = 0;
    for (int i = 0; i < 64; ++i) t->fd_table[i] = 0;

    /* ★ 第 18D 步任务 10：默认继承根命名空间 */
    extern struct pid_namespace *namespace_root_pidns(void);
    extern struct mnt_namespace *namespace_root_mntns(void);
    extern struct ipc_namespace *namespace_root_ipcns(void);
    extern void namespace_retain(struct task_t *t);

    t->pidns = namespace_root_pidns();
    t->mntns = namespace_root_mntns();
    t->ipcns = namespace_root_ipcns();
    namespace_retain(t);

    /* ★★★ 修复 2：内存配额初始化 ★★★
     *
     * 人工必须审查：
     *   - 权限 0/1 的配额与 MEM_DOMAIN_PRIVx_MAX_PAGES 保持一致；
     *   - 其他权限默认 0（不限制）；
     *   - mem_pages_used 从 0 开始，由 user_map_pages 累加，
     *     由 free_user_pages 递减。 */
    if (t->privilege_level == 0) {
        t->mem_quota_pages = 16;   /* = MEM_DOMAIN_PRIV0_MAX_PAGES */
    } else if (t->privilege_level == 1) {
        t->mem_quota_pages = 64;   /* = MEM_DOMAIN_PRIV1_MAX_PAGES */
    } else {
        t->mem_quota_pages = 0;
    }
    t->mem_pages_used = 0;
}

struct see_instance;
void see_cleanup_sandbox(struct task_t *cur);
struct see_instance *see_task_instance(struct task_t *t);
void see_destroy_instance(struct see_instance *inst);

void task_18d_cleanup(struct task_t *t)
{
    if (!t) return;

    /* 关闭所有 fd */
    for (int i = 3; i < 64; ++i) {
        if (t->fd_table[i]) {
            vfs_close(t->fd_table[i]);
            t->fd_table[i] = 0;
        }
    }

    /* 释放 shm 映射 */
    extern void shm_release_mappings(struct task_t *t);
    shm_release_mappings(t);

    extern void namespace_release(struct task_t *t);
    namespace_release(t);
}

int task_is_zombie(uint64_t tid)
{
    struct task_t *t = task_find_by_pid(tid);
    return t && t->is_zombie;
}

struct task_t *task_find_alive_by_pid(uint64_t pid)
{
    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);
    struct task_t *t = g_task_head;
    while (t) {
        if (t->pid == pid && !t->is_zombie) break;
        t = t->global_next;
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    return t;
}

struct task_t *task_list_head_raw(void)
{
    return g_task_head;
}

/*
 * task_join —— 等待目标 task 退出并回收其骨架。
 */
int task_join(uint64_t tid, int *out_code)
{
    struct task_t *self = task_from_thread(sched_current());
    if (!self) return OB_EPERM;
    if (tid == self->pid) return OB_EINVAL;

    /* ---- 第 1 轮：快速路径 ---- */
    struct task_t *target = task_find_by_pid(tid);
    if (!target) {
        if (out_code) *out_code = 0;
        return OB_ENOENT;
    }

    if (target->is_zombie) {
        int code = target->exit_code;
        if (out_code) *out_code = code;
        task_destroy(target);
        return 0;
    }

    /* ---- 第 2 轮：注册 waiter 并阻塞 ---- */
    struct futex_waiter waiter;
    {
        uint8_t *p = (uint8_t *)&waiter;
        for (unsigned i = 0; i < sizeof(waiter); ++i) p[i] = 0;
    }
    waiter.th            = sched_current();
    waiter.uaddr         = (uint64_t)(uintptr_t)target;
    waiter.pid           = self->pid;
    waiter.wake_deadline = 0;
    waiter.woken         = 0;
    waiter.next          = 0;

    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);

    if (target->is_zombie) {
        int code = target->exit_code;
        spin_unlock_irqrestore(&g_task_lock, flags);
        if (out_code) *out_code = code;
        task_destroy(target);
        return 0;
    }

    if (target->join_waiter) {
        spin_unlock_irqrestore(&g_task_lock, flags);
        return OB_EAGAIN;
    }

    target->join_waiter = &waiter;
    spin_unlock_irqrestore(&g_task_lock, flags);

    /* 阻塞 */
    waiter.th->state = THREAD_STATE_BLOCKED;
    schedule();

    /* ---- 第 3 轮：被唤醒后回收 ---- */
    if (waiter.woken) {
        struct task_t *t2 = task_find_by_pid(tid);
        if (t2 && t2->is_zombie) {
            int code = t2->exit_code;
            if (out_code) *out_code = code;
            serial_printf("[TASK] joined pid=%llu code=%d (reaped)\n",
                          (unsigned long long)tid, code);
            task_destroy(t2);
            return 0;
        }
        if (out_code) *out_code = 0;
        return OB_ENOENT;
    }

    if (out_code) *out_code = 0;
    return OB_EAGAIN;
}

static uint8_t inherit_privilege(uint8_t parent_priv)
{
    switch (parent_priv) {
    case 7: return 6;
    case 9: return 6;
    case 8: return 8;
    default: return parent_priv;
    }
}

uint32_t default_child_limit(uint8_t priv)
{
    if (priv == 0)        return 0;
    if (priv == 1)        return 1;
    if (priv <= 4)        return 16;
    if (priv <= 6)        return 32;
    if (priv <= 8)        return 128;
    return 0xFFFFFFFFu;
}

uint8_t compute_is_critical(uint64_t pid, uint8_t priv,
                            uint8_t sandbox_flags,
                            int     is_privileged_creation)
{
    if (sandbox_flags != 0) return 0;
    if (pid >= PID_RESERVED_MIN && pid <= PID_RESERVED_MAX) return 1;
    if (priv == PRIV_KERNEL_MANAGER && is_privileged_creation) return 1;
    return 0;
}

void task_init(void)
{
    g_task_head  = 0;
    g_task_count = 0;

    uint8_t *p = (uint8_t *)&g_bootstrap_task_storage;
    for (size_t i = 0; i < sizeof(g_bootstrap_task_storage); ++i) p[i] = 0;

    g_bootstrap_task_storage.pid                = PID_IDLE;
    g_bootstrap_task_storage.parent_pid         = PID_IDLE;
    g_bootstrap_task_storage.privilege_level    = PRIV_KERNEL_MANAGER;
    g_bootstrap_task_storage.isolation_mode     = ISOLATION_NONE;
    g_bootstrap_task_storage.sandbox_flags      = 0;
    g_bootstrap_task_storage.is_critical        = 0;
    g_bootstrap_task_storage.cpu_usage_quota    = 100;
    g_bootstrap_task_storage.child_process_limit= 128;
    g_bootstrap_task_storage.children_count     = 0;
    g_bootstrap_task_storage.child_head         = 0;
    g_bootstrap_task_storage.sibling_next       = 0;
    g_bootstrap_task_storage.global_next        = 0;
    g_bootstrap_task_storage.thread             = 0;
    g_bootstrap_task_storage.syscall_table_ptr  = 0;
    g_bootstrap_task_storage.ui_token_valid     = 0;
    g_bootstrap_task_storage.name               = "bootstrap";
    g_bootstrap_task_storage.exit_code          = 0;
    g_bootstrap_task_storage.security_token.level = PRIV_KERNEL_MANAGER;
    g_bootstrap_task_storage.priv_iso_ready     = 0;
    g_bootstrap_task_storage.compat_region      = 0;
    g_bootstrap_task_storage.compat_type        = COMPAT_TYPE_NONE;
    g_bootstrap_task_storage.htab               = 0;
    g_bootstrap_task_storage.netns              = 0;
    g_bootstrap_task_storage.pgid               = PID_IDLE;
    g_bootstrap_task_storage.user_ctx           = 0;
    g_bootstrap_task_storage.futex_wake         = 0;

    /* ★ 修复 2：bootstrap 配额 */
    g_bootstrap_task_storage.mem_quota_pages    = 0;
    g_bootstrap_task_storage.mem_pages_used     = 0;

    g_task_head  = &g_bootstrap_task_storage;
    g_task_count = 1;

    serial_printf("[TASK] init: bootstrap pid=0 priv=9 critical=0\n");
}

struct task_t *task_create(const char *name,
                           void (*entry)(void *), void *arg,
                           struct task_t *parent,
                           uint64_t requested_pid,
                           uint8_t  requested_priv,
                           uint8_t  sandbox_flags,
                           int      privileged_creation)
{
    if (!entry) return 0;

    int explicit_parent = (parent != 0);
    struct task_t *par  = explicit_parent ? parent : &g_bootstrap_task_storage;

    uint8_t child_priv;
    if (explicit_parent) {
        child_priv = inherit_privilege(par->privilege_level);
        if (requested_priv < child_priv) child_priv = requested_priv;
    } else {
        child_priv = (requested_priv > PRIV_MAX) ? PRIV_MAX : requested_priv;
    }

    if (sandbox_flags != 0 && child_priv == 9) child_priv = 6;

    if (explicit_parent && par->privilege_level == 0) {
        serial_printf("[TASK] create '%s': parent is priv0 "
                      "(no children allowed)\n",
                      name ? name : "(null)");
        return 0;
    }

    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);

    if (par->children_count >= par->child_process_limit) {
        spin_unlock_irqrestore(&g_task_lock, flags);
        serial_printf("[TASK] create '%s': parent pid=%llu at child limit "
                      "(%u)\n",
                      name ? name : "(null)",
                      (unsigned long long)par->pid,
                      (unsigned)par->child_process_limit);
        return 0;
    }

    int64_t pid_or_err;
    if (privileged_creation &&
        requested_pid >= PID_RESERVED_MIN &&
        requested_pid <= PID_RESERVED_MAX) {
        pid_or_err = alloc_pid_kernel(requested_pid);
    } else {
        pid_or_err = alloc_pid(requested_pid);
    }
    if (pid_or_err < 0) {
        spin_unlock_irqrestore(&g_task_lock, flags);
        serial_printf("[TASK] create '%s': alloc_pid(%llu) failed rc=%lld\n",
                      name ? name : "(null)",
                      (unsigned long long)requested_pid,
                      (long long)pid_or_err);
        return 0;
    }
    uint64_t pid = (uint64_t)pid_or_err;

    struct task_t *t = (struct task_t *)kzalloc(sizeof(struct task_t));
    if (!t) {
        free_pid(pid);
        spin_unlock_irqrestore(&g_task_lock, flags);
        serial_printf("[TASK] create '%s': PCB alloc failed\n",
                      name ? name : "(null)");
        return 0;
    }

    t->pid                  = pid;
    t->parent_pid           = par->pid;
    t->privilege_level      = child_priv;
    t->isolation_mode       = ISOLATION_NONE;
    t->sandbox_flags        = sandbox_flags;
    t->is_critical          = compute_is_critical(pid, child_priv,
                                                   sandbox_flags,
                                                   privileged_creation);
    t->cpu_usage_quota      = 100;
    t->child_process_limit  = default_child_limit(child_priv);
    t->children_count       = 0;
    t->child_head           = 0;
    t->sibling_next         = 0;
    t->global_next          = 0;
    t->thread               = 0;
    t->syscall_table_ptr    = 0;
    t->ui_token_valid       = 0;
    t->name                 = name;
    t->exit_code            = 0;

    uint8_t *md = (uint8_t *)&t->mem_domain;
    for (size_t i = 0; i < sizeof(t->mem_domain); ++i) md[i] = 0;

    t->security_token.level = child_priv;
    t->security_token.uid   = 0;
    t->security_token.gid   = 0;
    t->security_token.flags = 0;
    for (int i = 0; i < 32; ++i) t->security_token.dir_whitelist_hash[i] = 0;

    t->priv0_sb_ptr          = 0;
    t->priv1_dir_path[0]     = '\0';
    t->mem_domain_pages      = 0;
    t->mem_domain_page_count = 0;
    t->priv_iso_ready        = 0;

    t->sandbox_sb_ptr        = 0;
    t->sandbox_dir_path[0]   = '\0';
    t->pending_kill          = 0;

    t->compat_region         = 0;
    compat_preload_inherit(t, par);

    t->compat_type = COMPAT_TYPE_NONE;
    t->htab        = 0;

    t->netns = par->netns;

    t->pgid       = pid;
    t->user_ctx   = 0;
    t->futex_wake = 0;

    /* ★ 修复 2：初始配额占位（由 task_18d_init 覆盖） */
    t->mem_quota_pages = 0;
    t->mem_pages_used  = 0;

    if (par && par->compat_type != COMPAT_TYPE_NONE) {
        t->compat_type = par->compat_type;
        if (par->htab) {
            t->htab = compat_handle_table_create();
            if (!t->htab) {
                if (t->compat_region) compat_preload_release(t);
                kfree(t);
                free_pid(pid);
                spin_unlock_irqrestore(&g_task_lock, flags);
                serial_printf("[TASK] create '%s': htab alloc failed\n",
                              name ? name : "(null)");
                return 0;
            }
            for (uint32_t i = 0; i < COMPAT_HANDLE_MAX; ++i) {
                if (par->htab->entries[i].in_use) {
                    compat_handle_alloc(t->htab,
                                        par->htab->entries[i].kernel_handle,
                                        par->htab->entries[i].type,
                                        par->htab->entries[i].flags);
                }
            }
        }
    }

    if (child_priv == 0 || child_priv == 1) {
        int iso_rc = mem_domain_create(t);
        if (iso_rc != 0) {
            if (t->htab) { compat_handle_table_destroy(t->htab); t->htab = 0; }
            if (t->compat_region) compat_preload_release(t);
            kfree(t);
            free_pid(pid);
            spin_unlock_irqrestore(&g_task_lock, flags);
            serial_printf("[TASK] create '%s': mem_domain_create "
                          "failed rc=%d\n",
                          name ? name : "(null)", iso_rc);
            return 0;
        }

        if (child_priv == 0) {
            iso_rc = priv0_mount_vfs(t);
            if (iso_rc != 0) {
                mem_domain_destroy(t);
                if (t->htab) { compat_handle_table_destroy(t->htab); t->htab = 0; }
                if (t->compat_region) compat_preload_release(t);
                kfree(t);
                free_pid(pid);
                spin_unlock_irqrestore(&g_task_lock, flags);
                serial_printf("[TASK] create '%s': priv0_mount_vfs "
                              "failed rc=%d\n",
                              name ? name : "(null)", iso_rc);
                return 0;
            }
        } else {
            iso_rc = priv1_setup_tmpdir(t);
            if (iso_rc != 0) {
                mem_domain_destroy(t);
                if (t->htab) { compat_handle_table_destroy(t->htab); t->htab = 0; }
                if (t->compat_region) compat_preload_release(t);
                kfree(t);
                free_pid(pid);
                spin_unlock_irqrestore(&g_task_lock, flags);
                serial_printf("[TASK] create '%s': priv1_setup_tmpdir "
                              "failed rc=%d\n",
                              name ? name : "(null)", iso_rc);
                return 0;
            }
        }
        t->priv_iso_ready = 1;
    }

    struct thread *th = thread_create(name, entry, arg, child_priv);
    if (!th) {
        if (t->priv_iso_ready) {
            if (child_priv == 0) priv0_umount_vfs(t);
            if (child_priv == 1) priv1_cleanup_tmpdir(t);
            mem_domain_destroy(t);
        }
        if (t->htab) { compat_handle_table_destroy(t->htab); t->htab = 0; }
        if (t->compat_region) compat_preload_release(t);
        kfree(t);
        free_pid(pid);
        spin_unlock_irqrestore(&g_task_lock, flags);
        serial_printf("[TASK] create '%s': thread_create failed\n",
                      name ? name : "(null)");
        return 0;
    }

    if (child_priv == 1) {
        if (th->slice_remaining > 1) {
            th->slice_remaining = th->slice_remaining / 4;
        }
        if (th->slice_remaining == 0) th->slice_remaining = 1;
    }

    t->thread  = th;
    th->task   = t;
    th->tid    = pid;

    t->global_next = g_task_head;
    g_task_head    = t;
    g_task_count++;

    t->sibling_next  = par->child_head;
    par->child_head  = t;
    par->children_count++;

    spin_unlock_irqrestore(&g_task_lock, flags);

    serial_printf("[TASK] created '%s' pid=%llu parent=%llu priv=%u "
                  "critical=%u sandbox=0x%x child_limit=%u\n",
                  name ? name : "(null)",
                  (unsigned long long)pid,
                  (unsigned long long)par->pid,
                  (unsigned)child_priv,
                  (unsigned)t->is_critical,
                  (unsigned)sandbox_flags,
                  (unsigned)t->child_process_limit);
    task_18d_init(t);
    return t;
}

/*
 * task_exit —— 任务退出（18D：zombie 语义 + 修复 7：orphan 标记）。
 */
void task_exit(int exit_code)
{
    struct task_t *self = task_from_thread(sched_current());

    if (!self) {
        thread_exit();
        for (;;) { __asm__ __volatile__("hlt"); }
    }

    /* ★ 1) 从所有 futex bucket 摘除自己 */
    futex_wake_all_for_thread(sched_current());

    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);

    self->exit_code = exit_code;

    /* ★ 2) 从父进程的 children 链摘除（不从 global_list 摘除） */
    if (self != &g_bootstrap_task_storage) {
        struct task_t *par = g_task_head;
        while (par && par->pid != self->parent_pid) par = par->global_next;
        if (par && par != self) {
            struct task_t **pp = &par->child_head;
            while (*pp && *pp != self) pp = &(*pp)->sibling_next;
            if (*pp == self) {
                *pp = self->sibling_next;
                if (par->children_count > 0) par->children_count--;
            }
        }
    }

    /* ★★★ 修复 7：parent_pid == 0 的 orphan 标记 ★★★
     *
     * 人工必须审查：
     *   - parent_pid == 0 表示由 bootstrap 创建的 task（如 init/secmgr/
     *     servicehost/auditd/user-driver），它们退出后无人 join。
     *   - 若不在 task_exit 内标记，这些 task 会永久保留 task_t 骨架、
     *     内核栈与 PID 泄漏。
     *   - 不能在 task_exit 内直接 task_destroy 当前 task（会释放当前线程
     *     的 task_t），因此保留 is_zombie = 1，由 idle 线程调用
     *     task_reap_orphans() 回收。 */
    int is_orphan = (self->parent_pid == 0 && self->pid != 0);
    (void)is_orphan;   /* 仅作说明，实际回收由 task_reap_orphans 负责 */

    self->is_zombie = 1;

    /* ★ 3) 取出 join_waiter；锁外唤醒 */
    struct futex_waiter *jw = self->join_waiter;
    self->join_waiter = 0;

    uint64_t pid = self->pid;
    spin_unlock_irqrestore(&g_task_lock, flags);

    if (jw) {
        jw->woken = 1;
        if (jw->th) {
            extern void sched_enqueue_thread(struct thread *th);
            sched_enqueue_thread(jw->th);
        }
    }

    /* ★ 4) 释放用户态资源（但保留 task_t 骨架） */
    user_teardown(self);

    if (self->sandbox_flags & OBSANDBOX_ACTIVE) {
        struct see_instance *inst = see_task_instance(self);
        see_cleanup_sandbox(self);
        if (inst) see_destroy_instance(inst);
        self->syscall_table_ptr = 0;
        self->sandbox_flags = 0;
    }

    if (self->priv_iso_ready) {
        if (self->privilege_level == 0) {
            priv0_umount_vfs(self);
        } else if (self->privilege_level == 1) {
            priv1_cleanup_tmpdir(self);
        }
        mem_domain_destroy(self);
        self->priv_iso_ready = 0;
    }

    if (self->compat_region) {
        compat_preload_release(self);
        self->compat_region = 0;
    }

    if (self->htab) {
        compat_handle_table_destroy(self->htab);
        self->htab = 0;
    }

    /* ★ 18D：关闭 fd、释放 shm 映射、释放命名空间引用 */
    task_18d_cleanup(self);

    serial_printf("[TASK] exit pid=%llu code=%d (zombie)\n",
                  (unsigned long long)pid, exit_code);

    /* 不 free_pid、不 kfree(task_t)、不释放内核栈 */
    thread_exit();
    for (;;) { __asm__ __volatile__("hlt"); }
}

/* ★★★ 修复 7：orphan zombie 回收 ★★★
 *
 * 人工必须审查：
 *   - 只回收 is_zombie && parent_pid == 0 && pid != 0 的 task。
 *   - 每次最多处理 8 个。
 *   - task_destroy 内部会从 g_task_head 摘除节点并释放 kstack/thread/
 *     task_t/PID。若目标已被 task_join 回收，本函数不会命中。
 *   - task_destroy 幂等：不会因为 task 已被部分释放而崩溃。
 */
void task_reap_orphans(void)
{
    struct task_t *victims[8];
    int n = 0;

    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);

    struct task_t *t = g_task_head;
    while (t && n < 8) {
        if (t->is_zombie && t->parent_pid == 0 && t->pid != 0) {
            victims[n++] = t;
        }
        t = t->global_next;
    }

    spin_unlock_irqrestore(&g_task_lock, flags);

    for (int i = 0; i < n; ++i) {
        serial_printf("[TASK] reaped pid=%llu code=%d\n",
                      (unsigned long long)victims[i]->pid,
                      victims[i]->exit_code);
        task_destroy(victims[i]);
    }
}

struct task_t *task_find_by_pid(uint64_t pid)
{
    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);
    struct task_t *t = g_task_head;
    while (t) {
        if (t->pid == pid) break;
        t = t->global_next;
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    return t;
}

void task_iterate(void (*cb)(struct task_t *t, void *arg), void *arg)
{
    if (!cb) return;

    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);

    struct task_t *t = g_task_head;
    while (t) {
        cb(t, arg);
        t = t->global_next;
    }

    spin_unlock_irqrestore(&g_task_lock, flags);
}

int check_pid_access(struct task_t *caller, uint64_t target_pid)
{
    if (!caller) return OB_EPERM;

    if (!pid_is_reserved(target_pid)) {
        return 0;
    }

    if (caller->pid <= PID_RESERVED_MAX) {
        return 0;
    }

    audit_pid_violation(caller->pid, target_pid);
    return OB_EPERM;
}

void task_destroy(struct task_t *t)
{
    if (!t) return;
    if (t == &g_bootstrap_task_storage) return;

    struct thread *th       = t->thread;
    uint64_t       pid      = t->pid;
    void          *kstack   = th ? th->kstack_base : 0;
    int            korder   = th ? th->kstack_order : -1;

    uint64_t flags;
    spin_lock_irqsave(&g_task_lock, &flags);

    {
        struct task_t *par = g_task_head;
        while (par && par->pid != t->parent_pid) par = par->global_next;
        if (par && par != t) {
            struct task_t **pp = &par->child_head;
            while (*pp && *pp != t) pp = &(*pp)->sibling_next;
            if (*pp == t) {
                *pp = t->sibling_next;
                if (par->children_count > 0) par->children_count--;
            }
        }
    }

    {
        struct task_t **gp = &g_task_head;
        while (*gp && *gp != t) gp = &(*gp)->global_next;
        if (*gp == t) {
            *gp = t->global_next;
            if (g_task_count > 0) g_task_count--;
        }
    }

    spin_unlock_irqrestore(&g_task_lock, flags);

    user_teardown(t);

    if (t->sandbox_flags & OBSANDBOX_ACTIVE) {
        struct see_instance *inst = see_task_instance(t);
        see_cleanup_sandbox(t);
        if (inst) see_destroy_instance(inst);
        t->syscall_table_ptr = 0;
        t->sandbox_flags = 0;
    }

    if (t->priv_iso_ready) {
        if (t->privilege_level == 0) {
            priv0_umount_vfs(t);
        } else if (t->privilege_level == 1) {
            priv1_cleanup_tmpdir(t);
        }
        mem_domain_destroy(t);
        t->priv_iso_ready = 0;
    }

    if (t->compat_region) {
        compat_preload_release(t);
    }

    if (t->htab) {
        compat_handle_table_destroy(t->htab);
        t->htab = 0;
    }

    task_18d_cleanup(t);

    t->compat_type = COMPAT_TYPE_NONE;
    t->netns = 0;

    if (pid != PID_IDLE) {
        free_pid(pid);
    }

    if (kstack && korder >= 0) {
        uint64_t stack_va = (uint64_t)(uintptr_t)kstack;
        uint64_t phys     = stack_va - DIRECTMAP_BASE;
        struct page *pg   = phys_to_page(phys);
        pmm_free_pages(pg, korder);
    }

    if (th) kfree(th);
    kfree(t);
}
/*===OmniBridgeOs/kernel/arch/x64/task.c 结束===*/