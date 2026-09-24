/*===OmniBridgeOs/kernel/arch/x64/see.c===*/
#include "see.h"
#include "see_policy.h"
#include "see_whitelist.h"
#include "audit.h"
#include "permission.h"
#include "serial.h"
#include "spinlock.h"
#include "sched.h"
#include "mem_domain.h"
#include "priv_iso.h"

/* 前向声明，避免 see.c 直接依赖 sandbox.h 造成循环 */
void sandbox_cleanup_fs(struct task_t *t);

static struct see_instance g_pool[SEE_MAX_INSTANCES];
static uint8_t g_pool_used[SEE_MAX_INSTANCES];

static struct see_policy_cache g_cache;
static spinlock_t g_see_lock = SPINLOCK_INIT;

static uint32_t g_next_sandbox_id = 1u;
static uint64_t g_total_intercepts = 0;
static uint64_t g_kill_count = 0;
static uint64_t g_cleanup_count = 0;

void see_init(void)
{
    for (uint32_t i = 0; i < SEE_MAX_INSTANCES; ++i) {
        uint8_t *p = (uint8_t *)&g_pool[i];
        for (uint32_t k = 0; k < sizeof(g_pool[i]); ++k) p[k] = 0;
        g_pool_used[i] = 0;
    }
    g_cache.generation = 0;
    g_cache.count      = 0;
    g_cache.head       = 0;
    g_next_sandbox_id  = 1u;
    g_total_intercepts = 0;
    g_kill_count       = 0;
    g_cleanup_count    = 0;
    spin_lock_init(&g_see_lock);

    see_policy_init();

    serial_printf("[SEE] init: policy cache ready, max_instances=%u\n",
                  (unsigned)SEE_MAX_INSTANCES);
}

int see_create_instance(uint64_t owner_pid, uint32_t flags,
                        struct see_instance **out)
{
    if (!out) return -1;
    *out = 0;

    uint64_t irqflags;
    spin_lock_irqsave(&g_see_lock, &irqflags);

    int slot = -1;
    for (uint32_t i = 0; i < SEE_MAX_INSTANCES; ++i) {
        if (!g_pool_used[i]) { slot = (int)i; break; }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&g_see_lock, irqflags);
        serial_printf("[SEE] create: pool exhausted\n");
        return -1;
    }

    struct see_instance *inst = &g_pool[slot];
    g_pool_used[slot] = 1;

    inst->sandbox_id   = g_next_sandbox_id++;
    inst->flags        = flags;
    inst->owner_pid    = owner_pid;
    inst->mem_limit    = 512ull * 1024ull * 1024ull;
    inst->cpu_quota    = 20;
    inst->timeout_ticks= 0;
    inst->whitelist_hash = 0;
    inst->next         = g_cache.head;
    see_policy_copy_default(&inst->policy);
    g_cache.head       = inst;
    g_cache.count++;
    g_cache.generation++;

    spin_unlock_irqrestore(&g_see_lock, irqflags);

    serial_printf("[SEE] instance created: id=%u owner=%llu flags=0x%x\n",
                  (unsigned)inst->sandbox_id,
                  (unsigned long long)owner_pid,
                  (unsigned)flags);
    *out = inst;
    return 0;
}

void see_destroy_instance(struct see_instance *inst)
{
    if (!inst) return;

    uint64_t irqflags;
    spin_lock_irqsave(&g_see_lock, &irqflags);

    struct see_instance **pp = &g_cache.head;
    while (*pp && *pp != inst) pp = &(*pp)->next;
    if (*pp == inst) {
        *pp = inst->next;
        if (g_cache.count > 0) g_cache.count--;
        g_cache.generation++;
    }

    for (uint32_t i = 0; i < SEE_MAX_INSTANCES; ++i) {
        if (&g_pool[i] == inst) {
            g_pool_used[i] = 0;
            uint8_t *p = (uint8_t *)inst;
            for (uint32_t k = 0; k < sizeof(*inst); ++k) p[k] = 0;
            break;
        }
    }

    spin_unlock_irqrestore(&g_see_lock, irqflags);
    serial_printf("[SEE] instance destroyed\n");
}

int see_install_default_policy(struct see_instance *inst)
{
    if (!inst) return OB_EINVAL;
    return see_whitelist_install_default(inst);
}

int see_is_sandbox(const struct task_t *t)
{
    return t && (t->sandbox_flags & OBSANDBOX_ACTIVE);
}

int see_bind_task(struct task_t *t, struct see_instance *inst)
{
    if (!t || !inst) return OB_EINVAL;
    t->syscall_table_ptr = inst;
    return 0;
}

struct see_instance *see_task_instance(struct task_t *t)
{
    if (!t) return 0;
    return (struct see_instance *)t->syscall_table_ptr;
}

static int syscall_to_res_type(uint64_t nr)
{
    switch (nr) {
    case SYS_OB_OpenFile:
    case SYS_OB_ReadFile:
    case SYS_OB_WriteFile:
    case SYS_OB_CloseHandle:
    case SYS_OB_LoadDriver:
    case SYS_OB_LoadDriverSandboxed:
    case SYS_OB_ReadKernelFile:
    case SYS_OB_InternalSign:
        return OB_RES_FILE;

    case SYS_OB_VirtualAlloc:
    case SYS_OB_VirtualFree:
    case SYS_OB_ReadKernelMemory:
    case SYS_OB_WriteKernelMemory:
        return OB_RES_MEMORY;

    case SYS_OB_CreateProcess:
    case SYS_OB_TerminateProcess:
    case SYS_OB_GetProcessInfo:
    case SYS_OB_SendSignal:
    case SYS_OB_CreateSandboxProcess:
        return OB_RES_PROCESS;

    case SYS_OB_Compat_Preload:
        return OB_RES_CONFIG;

    default:
        return -1;
    }
}

int64_t see_syscall_interceptor(struct syscall_frame *f, struct task_t *cur)
{
    if (!f || !cur) return OB_EPERM;

    g_total_intercepts++;

    uint64_t nr = f->rax;

    /* 1) 原生权限检查优先 */
    int res_type = syscall_to_res_type(nr);
    if (res_type >= 0) {
        int rc = check_permission(cur, res_type, 0, OB_ACCESS_READ, NULL);
        if (rc != 0) {
            serial_printf("[SEE] native check denied pid=%llu nr=0x%llx rc=%d\n",
                          (unsigned long long)cur->pid,
                          (unsigned long long)nr, rc);
            return rc;
        }
    }

    /* 2) 白名单检查 */
    struct see_instance *inst = see_task_instance(cur);
    if (!inst) {
        see_deny_handler(cur, nr, "sandbox instance missing");
        return OB_EPERM;
    }

    if (see_policy_check(inst, nr)) {
        return 0;
    }

    /* 3) 原生通过但白名单拒绝 => 逃逸 */
    see_deny_handler(cur, nr, "not in sandbox whitelist");
    see_kill_sandbox(cur, cur);
    return OB_EPERM;
}

void see_deny_handler(struct task_t *cur, uint64_t nr, const char *reason)
{
    uint64_t pid = cur ? cur->pid : 0;

    audit_event(AUDIT_EV_SANDBOX_ESCAPE, AUDIT_LVL_CRITICAL,
                pid, nr, 0, 0, reason);

    serial_printf("[SEE] DENY: pid=%llu nr=0x%llx reason=%s\n",
                  (unsigned long long)pid,
                  (unsigned long long)nr,
                  reason ? reason : "(null)");

    if (cur) {
        cur->exit_code = OB_EPERM;
        cur->pending_kill = 1;
    }
}

void see_cleanup_sandbox(struct task_t *cur)
{
    if (!cur) return;
    if (!(cur->sandbox_flags & OBSANDBOX_ACTIVE) && !cur->sandbox_sb_ptr)
        return;

    if (cur->sandbox_sb_ptr) {
        sandbox_cleanup_fs(cur);
    }

    if (cur->priv_iso_ready) {
        if (cur->privilege_level == 0) {
            priv0_umount_vfs(cur);
        } else if (cur->privilege_level == 1) {
            priv1_cleanup_tmpdir(cur);
        }
        mem_domain_destroy(cur);
        cur->priv_iso_ready = 0;
    }

    g_cleanup_count++;
    serial_printf("[SEE] cleanup: pid=%llu pages=%u done\n",
                  (unsigned long long)cur->pid,
                  (unsigned)cur->mem_domain_page_count);
}

int see_kill_sandbox(struct task_t *caller, struct task_t *target)
{
    if (!target) return OB_EINVAL;
    if (!(target->sandbox_flags & OBSANDBOX_ACTIVE)) return OB_EINVAL;

    if (caller && caller != target) {
        if (caller->privilege_level < 7 &&
            caller->pid != target->parent_pid) {
            audit_pid_violation(caller->pid, target->pid);
            return OB_EPERM;
        }
    }

    struct see_instance *inst = see_task_instance(target);
    see_cleanup_sandbox(target);
    if (inst) see_destroy_instance(inst);

    target->syscall_table_ptr = 0;
    target->sandbox_flags = 0;
    target->pending_kill = 1;
    target->exit_code = OB_EPERM;
    g_kill_count++;

    if (target == task_from_thread(sched_current())) {
        task_exit(target->exit_code);
        for (;;) { __asm__ __volatile__("hlt"); }
    }

    return 0;
}

uint64_t see_active_count(void)
{
    uint64_t irqflags;
    spin_lock_irqsave(&g_see_lock, &irqflags);
    uint64_t v = (uint64_t)g_cache.count;
    spin_unlock_irqrestore(&g_see_lock, irqflags);
    return v;
}

uint64_t see_total_intercepts(void)
{
    return g_total_intercepts;
}

uint64_t see_kill_count(void)
{
    return g_kill_count;
}

uint64_t see_cleanup_count(void)
{
    return g_cleanup_count;
}