/*===OmniBridgeOs/kernel/arch/x64/see_test.c===*/
#include "see_test.h"
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
        serial_printf("[SEE-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[SEE-TEST] FAIL: %s\n", desc);
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
    t->child_process_limit = 32;
    t->security_token.level = priv;
}

void see_test(void)
{
    failures = 0;

    see_init();
    check("init: active_count == 0", see_active_count() == 0);

    struct see_instance *i1 = 0;
    int rc = see_create_instance(2000, OBSANDBOX_ACTIVE, &i1);
    check("create instance #1", rc == 0 && i1 != 0);
    check("active_count == 1", see_active_count() == 1);

    struct see_instance *i2 = 0;
    rc = see_create_instance(2001, OBSANDBOX_USER, &i2);
    check("create instance #2", rc == 0 && i2 != 0);
    check("active_count == 2", see_active_count() == 2);

    see_destroy_instance(i1);
    check("active_count == 1", see_active_count() == 1);
    see_destroy_instance(i2);
    check("active_count == 0", see_active_count() == 0);

    struct task_t fake;
    make_fake(&fake, 2000, 5, OBSANDBOX_ACTIVE);

    struct see_instance *inst = 0;
    rc = see_create_instance(fake.pid, OBSANDBOX_ACTIVE, &inst);
    check("create instance for interceptor", rc == 0 && inst != 0);
    if (rc == 0 && inst) {
        see_whitelist_install_default(inst);
        see_bind_task(&fake, inst);

        struct syscall_frame f;
        uint8_t *pf = (uint8_t *)&f;
        for (unsigned i = 0; i < sizeof(f); ++i) pf[i] = 0;
        f.rax = SYS_OB_OpenFile;
        int64_t rc2 = see_syscall_interceptor(&f, &fake);
        check("interceptor allows whitelisted OpenFile", rc2 == 0);

        f.rax = SYS_OB_LoadDriver;
        uint64_t before = audit_total_count();
        rc2 = see_syscall_interceptor(&f, &fake);
        check("interceptor denies non-whitelisted LoadDriver",
              rc2 == OB_EPERM);
        check("deny records audit", audit_total_count() > before);
        check("deny sets pending_kill", fake.pending_kill == 1);
    }

    if (failures == 0) {
        serial_printf("[SEE-TEST] selftest OK\n");
    } else {
        serial_printf("[SEE-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}