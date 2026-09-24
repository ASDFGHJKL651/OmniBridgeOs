/*===OmniBridgeOs/kernel/arch/x64/compat_exception_test.c===*/
#include "compat_exception_test.h"
#include "compat_exception.h"
#include "task.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[COMPAT-EXCEPTION-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[COMPAT-EXCEPTION-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static void make_fake_task(struct task_t *t, uint64_t pid, uint32_t compat)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;
    t->pid = pid;
    t->compat_type = (uint8_t)compat;
}

static void make_fake_regs(struct regs *r, uint64_t vector,
                           uint64_t error, uint64_t rip)
{
    uint8_t *p = (uint8_t *)r;
    for (unsigned i = 0; i < sizeof(*r); ++i) p[i] = 0;
    r->vector = vector;
    r->error  = error;
    r->rip    = rip;
    r->cs     = 0x08;
    r->rflags = 0x202;
}

void compat_exception_test(void)
{
    failures = 0;

    struct regs r;
    struct task_t t;

    /* 1) 非兼容进程返回 0 */
    {
        make_fake_task(&t, 5000, COMPAT_TYPE_NONE);
        make_fake_regs(&r, 13, 0, 0x1000);
        int rc = compat_exception_handle(&r, &t);
        check("non-compat returns 0", rc == 0);
    }

    /* 2) WIN32 兼容进程返回 1 */
    {
        make_fake_task(&t, 5001, COMPAT_TYPE_WIN32);
        make_fake_regs(&r, 13, 0, 0x2000);
        int rc = compat_exception_handle(&r, &t);
        check("win32 returns 1", rc == 1);
    }

    /* 3) LINUX 兼容进程返回 1 */
    {
        make_fake_task(&t, 5002, COMPAT_TYPE_LINUX);
        make_fake_regs(&r, 6, 0, 0x3000);
        int rc = compat_exception_handle(&r, &t);
        check("linux returns 1", rc == 1);
    }

    /* 4) NULL 输入返回 0 */
    {
        int rc = compat_exception_handle(0, 0);
        check("NULL returns 0", rc == 0);
    }

    if (failures == 0) {
        serial_printf("[COMPAT-EXCEPTION-TEST] selftest OK\n");
    } else {
        serial_printf("[COMPAT-EXCEPTION-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}