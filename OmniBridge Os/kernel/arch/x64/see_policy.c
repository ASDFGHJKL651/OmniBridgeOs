/*===OmniBridgeOs/kernel/arch/x64/see_policy.c===*/
#include "see_policy.h"
#include "see.h"
#include "serial.h"
#include "vfs.h"

void see_policy_init(void)
{
    /* 幂等：无全局状态，仅打印一次初始化信息 */
    static int inited = 0;
    if (inited) return;
    inited = 1;
    serial_printf("[SEE-POLICY] init: max_rules=%u\n",
                  (unsigned)SEE_MAX_RULES);
}

int see_policy_add_rule(struct see_instance *inst,
                        uint64_t syscall_nr,
                        uint32_t res_type,
                        uint32_t access_mask)
{
    if (!inst) return OB_EINVAL;
    if (inst->policy.rule_count >= SEE_MAX_RULES) return OB_ENOMEM;

    struct see_rule *r = &inst->policy.rules[inst->policy.rule_count++];
    r->syscall_nr  = syscall_nr;
    r->res_type    = res_type;
    r->access_mask = access_mask;
    return 0;
}

int see_policy_check(const struct see_instance *inst, uint64_t syscall_nr)
{
    if (!inst) return 0;

    for (uint32_t i = 0; i < inst->policy.rule_count; ++i) {
        const struct see_rule *r = &inst->policy.rules[i];
        if (r->syscall_nr == 0 || r->syscall_nr == syscall_nr) {
            return 1;
        }
    }
    return 0;
}

void see_policy_copy_default(struct see_policy *dst)
{
    if (!dst) return;
    dst->rule_count   = 0;
    dst->mem_limit    = 512ULL * 1024ULL * 1024ULL;
    dst->cpu_quota    = 20;
    dst->timeout_ticks = 0;
    dst->max_children = 3;
}