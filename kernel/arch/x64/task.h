/*===OmniBridgeOs/kernel/arch/x64/task.h===*/
#ifndef OMNIBRIDGE_TASK_H
#define OMNIBRIDGE_TASK_H

#include <stdint.h>
#include <stddef.h>
#include "sched.h"

struct vfs_superblock;
struct compat_preload_region;
struct compat_handle_table;
struct netns;                     /* ★ 第 18A 步 */
struct futex_waiter;
struct shm_mapping;
struct seccomp_filter;
struct pid_namespace;
struct mnt_namespace;
struct ipc_namespace;

/* ★ 第 18D 步：zombie 状态用于 join 语义。
 *   struct thread.state 的取值（sched.h 定义 THREAD_STATE_*）：
 *     0 READY, 1 RUNNING, 2 BLOCKED, 3 DEAD
 *     4 ZOMBIE —— task_exit 后、join 前的状态 */
#define THREAD_STATE_ZOMBIE 4

/* ★ 第 18 步：兼容类型 */
#define COMPAT_TYPE_NONE  0
#define COMPAT_TYPE_WIN32 1
#define COMPAT_TYPE_LINUX 2

#define PID_IDLE              0ULL
#define PID_RESERVED_MIN      1ULL
#define PID_RESERVED_MAX      99ULL
#define PID_EXT_MIN           100ULL
#define PID_EXT_MAX           999ULL
#define PID_USER_MIN          1000ULL
#define PID_MAX               65535ULL

#define PRIV_MIN              0
#define PRIV_MAX              9
#define PRIV_KERNEL_MANAGER   9

#define ISOLATION_NONE        0
#define ISOLATION_MEM         1
#define ISOLATION_TMPDIR      2

#define OBSANDBOX_ACTIVE      0x01

#ifndef OB_EINVAL
#define OB_EINVAL             (-22)
#define OB_EPERM              (-1)
#define OB_EAGAIN             (-11)
#define OB_ENOSYS             (-38)
#endif

#ifndef OB_EIO
#define OB_EIO                (-5)
#endif
#ifndef OB_ENOENT
#define OB_ENOENT             (-2)
#endif
#ifndef OB_EACCES
#define OB_EACCES             (-13)
#endif
#ifndef OB_EBADF
#define OB_EBADF              (-9)
#endif
#ifndef OB_ENOMEM
#define OB_ENOMEM             (-12)
#endif
#ifndef OB_EFAULT
#define OB_EFAULT             (-14)   /* ★ 第 18C 步：bad user address */
#endif
#ifndef OB_ENETUNREACH
#define OB_ENETUNREACH        (-101)  /* ★ 第 18A 步 */
#endif

struct mem_domain_t {
    uint64_t base_vaddr;
    uint64_t limit_vaddr;
    uint64_t phys_frame_count;
    uint64_t *pml4_self_ptr;
    uint8_t  sandbox_owned;
};

typedef uint32_t ob_uid_t;
typedef uint32_t ob_gid_t;

struct token_t {
    uint8_t   level;
    ob_uid_t  uid;
    ob_gid_t  gid;
    uint8_t   dir_whitelist_hash[32];
    uint8_t   flags;
};

struct task_t {
    uint64_t pid;
    uint64_t parent_pid;

    uint8_t  privilege_level;
    uint8_t  isolation_mode;
    uint8_t  sandbox_flags;
    uint8_t  is_critical;

    struct mem_domain_t mem_domain;
    struct token_t      security_token;

    uint32_t cpu_usage_quota;
    uint32_t child_process_limit;
    uint32_t children_count;

    struct task_t *child_head;
    struct task_t *sibling_next;
    struct task_t *global_next;

    struct thread *thread;

    void     *syscall_table_ptr;
    uint8_t   ui_token_valid;

    const char *name;
    int        exit_code;

    struct vfs_superblock *priv0_sb_ptr;
    char     priv1_dir_path[128];
    void    *mem_domain_pages;
    uint32_t mem_domain_page_count;
    uint32_t _pad_domain;
    uint8_t  priv_iso_ready;
    uint8_t  _pad_iso[7];

    struct vfs_superblock *sandbox_sb_ptr;
    char     sandbox_dir_path[128];
    uint8_t  pending_kill;
    uint8_t  _pad_kill[7];

    /* 第 17 步：兼容预加载共享区域 */
    struct compat_preload_region *compat_region;

    /* ★ 第 18 步：兼容类型与句柄表 */
    uint8_t  compat_type;
    uint8_t  _pad_compat[7];
    struct compat_handle_table *htab;

    /* ★ 第 18A 步：网络命名空间 */
    struct netns *netns;

    /* ============================================================
     * ★ 第 18C 步：用户态运行时与作业控制
     *
     * 人工必须审查：
     *   - user_ctx 由 user.c 管理生命周期（task_create 后由 user_setup 填充，
     *     由 task_exit → user_teardown 释放）。
     *   - pgid 用于作业控制（终端的进程组）。对于内核线程，
     *     pgid == pid 表示独占一个进程组。
     *   - futex_wake 是简化 futex 实现的每任务唤醒计数（18D 将替换为
     *     每个 futex 地址维护等待队列）。
     * ============================================================ */
    uint64_t pgid;
    void    *user_ctx;               /* 指向 struct user_ctx */
    uint64_t futex_wake;
    /* ============================================================
     * ★ 第 18D 步新增
     *
     * 人工必须审查：
     *   - is_zombie: 1 表示 task 已退出但未被 join；task_t 骨架保留。
     *     task_join 将其回收（调用 task_destroy）。
     *   - join_waiter: 指向正在 task_join 等待本 task 的 waiter。
     *     由 futex 机制管理（一个 task 最多被一个 waiter join）。
     *   - fd_table: per-task fd 表（18C 是全局 g_fd_table）。
     *   - shm_list: 共享内存映射链表。
     *   - seccomp_*: seccomp 过滤器。
     *   - pidns/mntns/ipcns: 命名空间指针。
     *   - ptrace_*: ptrace 追踪信息。
     * ============================================================ */
    int      is_zombie;
    int      _pad_zombie;
    struct futex_waiter *join_waiter;

    /* per-task fd 表：64 个 vfs_file 指针 */
    struct vfs_file *fd_table[64];

    /* 共享内存映射链表 */
    struct shm_mapping *shm_list;

    /* seccomp */
    uint8_t  seccomp_mode;
    uint8_t  _pad_seccomp[7];
    struct seccomp_filter *seccomp_filter;

    /* 命名空间 */
    struct pid_namespace *pidns;
    struct mnt_namespace *mntns;
    struct ipc_namespace *ipcns;

    /* ptrace */
    uint32_t ptrace_flags;
    uint32_t _pad_ptrace;
    uint64_t ptrace_tracer_pid;
    /* ★★★ 修复 2：内存配额（页数） ★★★
     *
     * 人工必须审查：
     *   - 单位是页（4KB），不是字节。
     *   - 0 表示不限制（与 Linux cgroup 语义一致）。
     *   - mem_pages_used 只统计用户页（stack/TLS/sigtramp/heap/shm），
     *     不含内核栈、页表页。
     *   - 默认值：
     *       权限 0 → MEM_DOMAIN_PRIV0_MAX_PAGES = 16
     *       权限 1 → MEM_DOMAIN_PRIV1_MAX_PAGES = 64
     *       其他   → 0（不限制）
     *   - 由 task_18d_init() 初始化。 */
    uint32_t mem_quota_pages;
    uint32_t mem_pages_used;
};

static inline struct task_t *task_from_thread(struct thread *t)
{
    return t ? (struct task_t *)t->task : 0;
}

static inline struct thread *thread_from_task(struct task_t *tk)
{
    return tk ? tk->thread : 0;
}

void task_init(void);
void pid_init(void);

struct task_t *task_create(const char *name,
                           void (*entry)(void *), void *arg,
                           struct task_t *parent,
                           uint64_t requested_pid,
                           uint8_t  requested_priv,
                           uint8_t  sandbox_flags,
                           int      privileged_creation);

void task_exit(int exit_code);

/* ★ 第 18D 步：task_find_by_pid 会返回 zombie（已退出但未 join）。
 *   - 用作"进程是否存活"判断时，请改用 task_find_alive_by_pid。
 *   - task_join / task_destroy 内部仍使用 task_find_by_pid。 */
struct task_t *task_find_by_pid(uint64_t pid);

/* ★ 第 18D 步新增：只返回非 zombie 的 task。
 *   语义：等价于 18C 之前的 task_find_by_pid。
 *   典型用途：轮询等待子进程结束。 */
struct task_t *task_find_alive_by_pid(uint64_t pid);

/* ★ 第 18D 步新增：判断 tid 是否为 zombie（存在但 is_zombie==1）。 */
int task_is_zombie(uint64_t tid);

/* ★ 第 18D 步新增：等待目标 task 退出并回收其骨架。
 *   成功返回 0 并写入 exit_code；失败返回负错误码。 */
int task_join(uint64_t tid, int *out_code);

/* ★ 第 18D 步新增：task_t 创建后初始化 18D 新字段。 */
void task_18d_init(struct task_t *t);

/* ★ 第 18D 步新增：任务退出时释放 fd 表、shm 映射、命名空间。 */
void task_18d_cleanup(struct task_t *t);

/* ★ 第 18D 步新增：供 oom.c 使用，返回全局 task 链表头（不取锁）。 */
struct task_t *task_list_head_raw(void);

void task_iterate(void (*cb)(struct task_t *t, void *arg), void *arg);
int check_pid_access(struct task_t *caller, uint64_t target_pid);
uint32_t default_child_limit(uint8_t priv);
uint8_t compute_is_critical(uint64_t pid, uint8_t priv,
                            uint8_t sandbox_flags,
                            int     is_privileged_creation);

void audit_pid_violation(uint64_t caller_pid, uint64_t target_pid);
void task_destroy(struct task_t *t);

/* ★ 第 18C 步：user.c 导出的"用户态入口占位"。
 * task_create 时若 entry == user_entry_marker，则创建的是用户态进程；
 * 由调用者（user_spawn_program）后续调用 user_setup 填充用户上下文。 */
void user_entry_marker(void *arg);

/* ★ 第 18D 步：join 等待线程退出并回收。
 * 成功返回 0 并写入 exit_code；失败返回负错误码。 */
int task_join(uint64_t tid, int *out_code);

/* ★ 第 18D 步：判断 tid 是否为 zombie。 */
int task_is_zombie(uint64_t tid);

/* ★ 第 18D 步：task_t 创建后初始化 18D 新字段（task_create 调用）。 */
void task_18d_init(struct task_t *t);

/* ★ 第 18D 步：任务退出时释放 fd 表、shm 映射。 */
void task_18d_cleanup(struct task_t *t);

struct task_t *task_list_head_raw(void);

#endif /* OMNIBRIDGE_TASK_H */
/*===OmniBridgeOs/kernel/arch/x64/task.h 结束===*/