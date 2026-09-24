/*===OmniBridgeOs/kernel/arch/x64/namespace.c===*/
#include "namespace.h"
#include "kmalloc.h"
#include "serial.h"
#include "vfs.h"
#include "task.h"
#include "sched.h"
#include "spinlock.h"

static struct pid_namespace *g_root_pidns;
static struct mnt_namespace *g_root_mntns;
static struct ipc_namespace *g_root_ipcns;
static uint32_t g_next_id = 1;

struct pid_namespace *namespace_root_pidns(void) { return g_root_pidns; }
struct mnt_namespace *namespace_root_mntns(void) { return g_root_mntns; }
struct ipc_namespace *namespace_root_ipcns(void) { return g_root_ipcns; }

static struct pid_namespace *alloc_pidns(struct pid_namespace *parent)
{
    struct pid_namespace *ns = (struct pid_namespace *)kzalloc(sizeof(*ns));
    if (!ns) return 0;
    ns->id = g_next_id++;
    ns->refcount = 1;
    ns->parent = parent;
    spin_lock_init(&ns->lock);

    /* ★ 修复 8：初始化本地 PID 映射表 */
    ns->map_count      = 0;
    ns->next_local_pid = 1;
    for (uint32_t i = 0; i < PIDNS_MAX_ENTRIES; ++i) {
        ns->map[i].global_pid = 0;
        ns->map[i].local_pid  = 0;
    }
    return ns;
}

static struct mnt_namespace *alloc_mntns(void)
{
    struct mnt_namespace *ns = (struct mnt_namespace *)kzalloc(sizeof(*ns));
    if (!ns) return 0;
    ns->id = g_next_id++;
    ns->refcount = 1;
    ns->mounts = 0;
    spin_lock_init(&ns->lock);
    return ns;
}

static struct ipc_namespace *alloc_ipcns(void)
{
    struct ipc_namespace *ns = (struct ipc_namespace *)kzalloc(sizeof(*ns));
    if (!ns) return 0;
    ns->id = g_next_id++;
    ns->refcount = 1;
    spin_lock_init(&ns->lock);
    return ns;
}

static void put_pidns(struct pid_namespace *ns)
{
    if (!ns) return;
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    if (ns->refcount > 0) ns->refcount--;
    uint32_t rc = ns->refcount;
    spin_unlock_irqrestore(&ns->lock, fl);
    if (rc == 0 && ns->parent != 0) {
        put_pidns(ns->parent);
        kfree(ns);
    }
}

static void put_mntns(struct mnt_namespace *ns)
{
    if (!ns) return;
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    if (ns->refcount > 0) ns->refcount--;
    uint32_t rc = ns->refcount;
    spin_unlock_irqrestore(&ns->lock, fl);
    if (rc == 0) kfree(ns);
}

static void put_ipcns(struct ipc_namespace *ns)
{
    if (!ns) return;
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    if (ns->refcount > 0) ns->refcount--;
    uint32_t rc = ns->refcount;
    spin_unlock_irqrestore(&ns->lock, fl);
    if (rc == 0) kfree(ns);
}

void namespace_init(void)
{
    g_root_pidns = alloc_pidns(0);
    g_root_mntns = alloc_mntns();
    g_root_ipcns = alloc_ipcns();

    if (g_root_pidns) g_root_pidns->refcount = 1000;
    if (g_root_mntns) g_root_mntns->refcount = 1000;
    if (g_root_ipcns) g_root_ipcns->refcount = 1000;

    serial_printf("[NS] init: pidns=%u mntns=%u ipcns=%u\n",
                  (unsigned)(g_root_pidns ? g_root_pidns->id : 0),
                  (unsigned)(g_root_mntns ? g_root_mntns->id : 0),
                  (unsigned)(g_root_ipcns ? g_root_ipcns->id : 0));
}

void namespace_retain(struct task_t *t)
{
    if (!t) return;
    if (t->pidns) { uint64_t f; spin_lock_irqsave(&t->pidns->lock, &f);
                    t->pidns->refcount++; spin_unlock_irqrestore(&t->pidns->lock, f); }
    if (t->mntns) { uint64_t f; spin_lock_irqsave(&t->mntns->lock, &f);
                    t->mntns->refcount++; spin_unlock_irqrestore(&t->mntns->lock, f); }
    if (t->ipcns) { uint64_t f; spin_lock_irqsave(&t->ipcns->lock, &f);
                    t->ipcns->refcount++; spin_unlock_irqrestore(&t->ipcns->lock, f); }
}

void namespace_release(struct task_t *t)
{
    if (!t) return;
    if (t->pidns) { put_pidns(t->pidns); t->pidns = 0; }
    if (t->mntns) { put_mntns(t->mntns); t->mntns = 0; }
    if (t->ipcns) { put_ipcns(t->ipcns); t->ipcns = 0; }
}

/* ★ 修复 8：pidns 相对化 */
uint64_t pidns_to_local(struct pid_namespace *ns, uint64_t global_pid)
{
    if (!ns) return global_pid;

    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);

    /* 查表 */
    for (uint32_t i = 0; i < ns->map_count; ++i) {
        if (ns->map[i].global_pid == global_pid) {
            uint64_t local = ns->map[i].local_pid;
            spin_unlock_irqrestore(&ns->lock, fl);
            return local;
        }
    }

    /* 新建 */
    if (ns->map_count < PIDNS_MAX_ENTRIES) {
        uint64_t local = ns->next_local_pid++;
        ns->map[ns->map_count].global_pid = global_pid;
        ns->map[ns->map_count].local_pid  = local;
        ns->map_count++;
        spin_unlock_irqrestore(&ns->lock, fl);
        return local;
    }

    /* 表满：退化为返回 global_pid */
    spin_unlock_irqrestore(&ns->lock, fl);
    return global_pid;
}

/* ★ 修复 9：namespace_fork —— 为 child 装配命名空间 */
int namespace_fork(struct task_t *cur, struct task_t *child, uint64_t flags)
{
    if (!cur || !child) return -22;

    /* PID namespace */
    if (flags & CLONE_NEWPID) {
        child->pidns = alloc_pidns(cur->pidns);
        if (!child->pidns) return -12;
    } else {
        child->pidns = cur->pidns;
        if (child->pidns) {
            uint64_t fl;
            spin_lock_irqsave(&child->pidns->lock, &fl);
            child->pidns->refcount++;
            spin_unlock_irqrestore(&child->pidns->lock, fl);
        }
    }

    /* Mount namespace */
    if (flags & CLONE_NEWNS) {
        child->mntns = alloc_mntns();
        if (!child->mntns) return -12;
    } else {
        child->mntns = cur->mntns;
        if (child->mntns) {
            uint64_t fl;
            spin_lock_irqsave(&child->mntns->lock, &fl);
            child->mntns->refcount++;
            spin_unlock_irqrestore(&child->mntns->lock, fl);
        }
    }

    /* IPC namespace */
    if (flags & CLONE_NEWIPC) {
        child->ipcns = alloc_ipcns();
        if (!child->ipcns) return -12;
    } else {
        child->ipcns = cur->ipcns;
        if (child->ipcns) {
            uint64_t fl;
            spin_lock_irqsave(&child->ipcns->lock, &fl);
            child->ipcns->refcount++;
            spin_unlock_irqrestore(&child->ipcns->lock, fl);
        }
    }

    return 0;
}

/*
 * ★ 修复 9：namespace_clone 语义变更
 *
 * 人工必须审查：
 *   - 原 namespace_clone 直接修改当前进程的命名空间指针，等同于 unshare。
 *   - 新语义：委托给 namespace_clone_child，创建真正的子进程。
 *   - 保留函数名以免破坏调用方（如 oshell）的链接。
 */
int64_t namespace_clone(struct task_t *cur, uint64_t flags)
{
    return namespace_clone_child(cur, flags);
}

/* ★ 修复 9：unshare 保持原语义 */
int64_t namespace_unshare(struct task_t *cur, uint64_t flags)
{
    if (!cur) return -1;
    if (cur->privilege_level < 2) return -1;

    if (flags & CLONE_NEWPID) {
        struct pid_namespace *ns = alloc_pidns(cur->pidns);
        if (!ns) return -12;
        put_pidns(cur->pidns);
        cur->pidns = ns;
    }
    if (flags & CLONE_NEWNS) {
        struct mnt_namespace *ns = alloc_mntns();
        if (!ns) return -12;
        put_mntns(cur->mntns);
        cur->mntns = ns;
    }
    if (flags & CLONE_NEWIPC) {
        struct ipc_namespace *ns = alloc_ipcns();
        if (!ns) return -12;
        put_ipcns(cur->ipcns);
        cur->ipcns = ns;
    }
    return (int64_t)cur->pid;
}

/*
 * ★ 修复 9：clone 子进程实现
 *
 * 人工必须审查：
 *   - 18D 阶段 fork 语义简化：创建内核线程作为"子进程"，
 *     共享父进程的 user_ctx（CLONE_VM 隐含），不复制用户内存。
 *   - 真正的 fork（复制用户地址空间）留待 19 步。
 *   - 返回 child->pid（全局 PID）。
 */
int64_t namespace_clone_child(struct task_t *cur, uint64_t flags)
{
    if (!cur) return -1;
    if (cur->privilege_level < 2) return -1;

    extern struct task_t *task_create(const char *name,
                                       void (*entry)(void *),
                                       void *arg,
                                       struct task_t *parent,
                                       uint64_t requested_pid,
                                       uint8_t requested_priv,
                                       uint8_t sandbox_flags,
                                       int privileged_creation);
    extern void user_entry_marker(void *arg);

    struct task_t *child = task_create("clone-child",
                                        user_entry_marker,
                                        0,
                                        cur,
                                        0,
                                        cur->privilege_level,
                                        0,
                                        0);
    if (!child) return OB_EAGAIN;

    int rc = namespace_fork(cur, child, flags);
    if (rc != 0) {
        extern void task_destroy(struct task_t *t);
        task_destroy(child);
        return rc;
    }

    /* 共享父进程的 user_ctx（CLONE_VM 隐含） */
    if (cur->user_ctx) {
        child->user_ctx = cur->user_ctx;
        /* 引用计数留待 19 步实现 */
    }

    serial_printf("[NS] clone: parent=%llu child=%llu flags=0x%llx\n",
                  (unsigned long long)cur->pid,
                  (unsigned long long)child->pid,
                  (unsigned long long)flags);

    return (int64_t)child->pid;
}
/*===OmniBridgeOs/kernel/arch/x64/namespace.c 结束===*/