/*===OmniBridgeOs/kernel/arch/x64/namespace.h===*/
#ifndef OMNIBRIDGE_NAMESPACE_H
#define OMNIBRIDGE_NAMESPACE_H

#include <stdint.h>
#include "spinlock.h"
#include "task.h"

#define CLONE_NEWNS   0x00020000u
#define CLONE_NEWPID  0x20000000u
#define CLONE_NEWIPC  0x08000000u
#define CLONE_NEWNET  0x40000000u
#define CLONE_VM      0x00000100u

/* ★★★ 修复 8：pidns 本地 PID 映射表容量 ★★★ */
#define PIDNS_MAX_ENTRIES  64

struct pid_namespace {
    uint32_t id;
    uint32_t refcount;
    struct pid_namespace *parent;
    spinlock_t lock;

    /* ★ 修复 8：本地 PID 映射表（global_pid → local_pid） */
    struct {
        uint64_t global_pid;
        uint64_t local_pid;
    } map[PIDNS_MAX_ENTRIES];
    uint32_t map_count;
    uint64_t next_local_pid;   /* 从 1 开始（0 保留给 idle） */
};

struct mnt_namespace {
    uint32_t id;
    uint32_t refcount;
    struct vfs_superblock *mounts;
    spinlock_t lock;
};

struct ipc_namespace {
    uint32_t id;
    uint32_t refcount;
    spinlock_t lock;
};

void namespace_init(void);

/* ★ 修复 9：namespace_clone 现在委托给 namespace_clone_child（fork）。
 *   namespace_unshare 保持原语义（切换当前进程命名空间）。 */
int64_t namespace_clone(struct task_t *cur, uint64_t flags);
int64_t namespace_unshare(struct task_t *cur, uint64_t flags);

/* ★ 修复 9：为 child 装配命名空间（fork 语义）。
 *   成功返回 0；失败返回负错误码（调用者需回滚 child 的创建）。 */
int namespace_fork(struct task_t *cur, struct task_t *child, uint64_t flags);

/* ★ 修复 9：clone 系统调用实现（创建子进程 + 装配命名空间）。 */
int64_t namespace_clone_child(struct task_t *cur, uint64_t flags);

void namespace_retain(struct task_t *t);
void namespace_release(struct task_t *t);

/* ★ 修复 8：global_pid → 当前 pidns 的 local_pid。
 *   - 若 global_pid 已在映射表中，返回已分配的 local_pid。
 *   - 否则新建映射（从 next_local_pid 递增）。
 *   - 映射表满时退化为返回 global_pid（不阻塞、不错误）。
 *
 *   人工必须审查：
 *     - 本函数只做单向映射（global → local），反向映射留待 19 步。
 *     - 根命名空间的 next_local_pid 起始值应为 1。 */
uint64_t pidns_to_local(struct pid_namespace *ns, uint64_t global_pid);

#endif /* OMNIBRIDGE_NAMESPACE_H */
/*===OmniBridgeOs/kernel/arch/x64/namespace.h 结束===*/