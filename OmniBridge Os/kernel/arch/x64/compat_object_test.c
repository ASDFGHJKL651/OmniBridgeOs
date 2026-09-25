/*===OmniBridgeOs/kernel/arch/x64/compat_object_test.c===*/
#include "compat_object_test.h"
#include "compat_object.h"
#include "audit.h"
#include "task.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[COMPAT-OBJECT-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[COMPAT-OBJECT-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void compat_object_test(void)
{
    failures = 0;

    compat_object_init();

    /* 1) #PF -> SIGSEGV */
    {
        struct compat_signal_info info;
        int rc = compat_signal_translate(14, 0x2, 0xDEADBEEF, 0x1000, &info);
        check("translate PF -> SIGSEGV",
              rc == 0 && info.signo == OB_SIGSEGV &&
              info.fault_addr == 0xDEADBEEF);
    }

    /* 2) #DE -> SIGFPE */
    {
        struct compat_signal_info info;
        int rc = compat_signal_translate(0, 0x0, 0, 0x2000, &info);
        check("translate DE -> SIGFPE",
              rc == 0 && info.signo == OB_SIGFPE);
    }

    /* 3) #UD -> SIGILL */
    {
        struct compat_signal_info info;
        int rc = compat_signal_translate(6, 0x0, 0, 0x3000, &info);
        check("translate UD -> SIGILL", rc == 0 && info.signo == OB_SIGILL);
    }

    /* 4) dispatch 记录审计 */
    {
        audit_init();
        struct task_t fake;
        uint8_t *p = (uint8_t *)&fake;
        for (unsigned i = 0; i < sizeof(fake); ++i) p[i] = 0;
        fake.pid = 8000;
        fake.compat_type = COMPAT_TYPE_LINUX;

        struct compat_signal_info info;
        compat_signal_translate(14, 0x2, 0xDEADBEEF, 0x1000, &info);

        uint64_t before = audit_total_count();
        int rc = compat_signal_dispatch(&fake, &info);
        check("dispatch records audit",
              rc == 0 && audit_total_count() > before);
    }

    /* 5) dump 不崩溃 */
    compat_object_dump();

    if (failures == 0) {
        serial_printf("[COMPAT-OBJECT-TEST] selftest OK\n");
    } else {
        serial_printf("[COMPAT-OBJECT-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}