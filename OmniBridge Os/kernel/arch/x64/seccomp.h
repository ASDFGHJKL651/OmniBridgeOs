/*===OmniBridgeOs/kernel/arch/x64/seccomp.h===*/
#ifndef OMNIBRIDGE_SECCOMP_H
#define OMNIBRIDGE_SECCOMP_H

#include <stdint.h>
#include "task.h"

#define SECCOMP_MODE_DISABLED  0
#define SECCOMP_MODE_FILTER    1

#define SECCOMP_RET_ALLOW      0
#define SECCOMP_RET_ERRNO      1
#define SECCOMP_RET_KILL       2

/* 位图覆盖 0x100~0x17F 共 128 个系统调用号 */
struct seccomp_filter {
    uint8_t  bitmap[16];
    uint32_t default_action;
    uint32_t errno_value;
};

void seccomp_init(void);

/* 系统调用主体：用户态通过 SYS_OB_Seccomp 设置。 */
int64_t seccomp_syscall(struct task_t *cur, uint64_t mode, uint64_t filt_uaddr);

/* 入口检查：返回 0 表示放行；负值表示拒绝（返回值由 default_action 决定）。 */
int seccomp_check(struct task_t *cur, uint64_t nr);

#endif /* OMNIBRIDGE_SECCOMP_H */