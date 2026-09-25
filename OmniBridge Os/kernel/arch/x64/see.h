/*===OmniBridgeOs/kernel/arch/x64/see.h===*/
#ifndef OMNIBRIDGE_SEE_H
#define OMNIBRIDGE_SEE_H

#include <stdint.h>
#include "syscall.h"
#include "task.h"
#include "see_policy.h"

#ifndef OBSANDBOX_ACTIVE
#define OBSANDBOX_ACTIVE  0x01u
#endif
#ifndef OBSANDBOX_KERNEL
#define OBSANDBOX_KERNEL  0x02u
#endif
#ifndef OBSANDBOX_USER
#define OBSANDBOX_USER    0x04u
#endif
#ifndef OBSANDBOX_NONE
#define OBSANDBOX_NONE    0x00u
#endif

#define SEE_MAX_INSTANCES 32u

struct see_instance {
    uint32_t  sandbox_id;
    uint32_t  flags;
    uint64_t  owner_pid;
    uint64_t  mem_limit;
    uint32_t  cpu_quota;
    uint32_t  timeout_ticks;
    uint64_t  whitelist_hash;
    struct see_policy policy;
    struct see_instance *next;
};

struct see_policy_cache {
    volatile uint64_t generation;
    volatile uint32_t count;
    struct see_instance *head;
};

void see_init(void);

int  see_create_instance(uint64_t owner_pid, uint32_t flags,
                         struct see_instance **out);
void see_destroy_instance(struct see_instance *inst);

int64_t see_syscall_interceptor(struct syscall_frame *f, struct task_t *cur);

void see_deny_handler(struct task_t *cur, uint64_t nr, const char *reason);

void see_cleanup_sandbox(struct task_t *cur);

uint64_t see_active_count(void);
uint64_t see_total_intercepts(void);

/* ★ 第 16 步新增 */
int see_install_default_policy(struct see_instance *inst);
int see_is_sandbox(const struct task_t *t);
int see_bind_task(struct task_t *t, struct see_instance *inst);
struct see_instance *see_task_instance(struct task_t *t);
int see_kill_sandbox(struct task_t *caller, struct task_t *target);
uint64_t see_kill_count(void);
uint64_t see_cleanup_count(void);

#endif /* OMNIBRIDGE_SEE_H */