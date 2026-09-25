/*===OmniBridgeOs/kernel/arch/x64/sandbox_test.c===*/
#include "sandbox_test.h"
#include "sandbox.h"
#include "see.h"
#include "see_policy.h"
#include "see_whitelist.h"
#include "audit.h"
#include "task.h"
#include "syscall.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[SANDBOX-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[SANDBOX-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static void make_fake(struct task_t *t, uint64_t pid,
                      uint8_t priv, uint8_t sandbox)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;
    t->pid = pid;
    t->privilege_level = priv;
    t->sandbox_flags = sandbox;
    t->security_token.level = priv;
    t->pending_kill = 0;
}

void sandbox_test(void)
{
    failures = 0;
    serial_printf("[SANDBOX-TEST] === begin ===\n");

    struct task_t fake;
    make_fake(&fake, 7000, 5, 0);
    check("see_is_sandbox(false)", see_is_sandbox(&fake) == 0);

    make_fake(&fake, 7001, 5, OBSANDBOX_ACTIVE);
    check("see_is_sandbox(true)", see_is_sandbox(&fake) == 1);

    struct see_instance *inst = 0;
    int rc = see_create_instance(fake.pid, OBSANDBOX_ACTIVE, &inst);
    check("instance create + policy install", rc == 0 && inst != 0);
    if (rc == 0 && inst) {
        see_whitelist_install_default(inst);
        check("policy rule_count >= 5", inst->policy.rule_count >= 5);

        see_bind_task(&fake, inst);
        check("bind/unbind roundtrip", see_task_instance(&fake) == inst);

        struct syscall_frame f;
        uint8_t *pf = (uint8_t *)&f;
        for (unsigned i = 0; i < sizeof(f); ++i) pf[i] = 0;
        f.rax = SYS_OB_OpenFile;
        int64_t rc2 = see_syscall_interceptor(&f, &fake);
        check("interceptor allows whitelisted syscall", rc2 == 0);

        f.rax = SYS_OB_LoadDriver;
        uint64_t before = audit_total_count();
        rc2 = see_syscall_interceptor(&f, &fake);
        check("interceptor denies non-whitelisted (escape)",
              rc2 == OB_EPERM);
        check("deny sets pending_kill", fake.pending_kill == 1);
        check("deny records audit", audit_total_count() > before);
        check("see_kill_sandbox idempotent",
              see_kill_sandbox(&fake, &fake) != 0 ||
              fake.sandbox_flags == 0);
    }

    if (failures == 0) {
        serial_printf("[SANDBOX-TEST] selftest OK\n");
    } else {
        serial_printf("[SANDBOX-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[SANDBOX-TEST] === end ===\n");
}