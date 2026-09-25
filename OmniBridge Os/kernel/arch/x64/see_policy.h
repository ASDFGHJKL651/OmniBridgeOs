/*===OmniBridgeOs/kernel/arch/x64/see_policy.h===*/
#ifndef OMNIBRIDGE_SEE_POLICY_H
#define OMNIBRIDGE_SEE_POLICY_H

#include <stdint.h>

struct see_instance;

#define SEE_MAX_RULES 32

struct see_rule {
    uint64_t syscall_nr;       /* 允许的调用号；0 表示“任意” */
    uint32_t res_type;         /* OB_RES_*；0 表示“不检查” */
    uint32_t access_mask;      /* OB_ACCESS_* 掩码 */
};

struct see_policy {
    struct see_rule rules[SEE_MAX_RULES];
    uint32_t  rule_count;
    uint64_t  mem_limit;       /* 字节 */
    uint32_t  cpu_quota;       /* 百分比 */
    uint32_t  timeout_ticks;   /* 0 = 无超时 */
    uint32_t  max_children;    /* 子进程上限，默认 3 */
};

void see_policy_init(void);

int see_policy_add_rule(struct see_instance *inst,
                        uint64_t syscall_nr,
                        uint32_t res_type,
                        uint32_t access_mask);

int see_policy_check(const struct see_instance *inst, uint64_t syscall_nr);

void see_policy_copy_default(struct see_policy *dst);

#endif /* OMNIBRIDGE_SEE_POLICY_H */