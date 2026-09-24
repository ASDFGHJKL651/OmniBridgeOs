/*===OmniBridgeOs/kernel/arch/x64/see_policy_test.c===*/
#include "see_policy_test.h"
#include "see_policy.h"
#include "see_whitelist.h"
#include "see.h"
#include "syscall.h"
#include "serial.h"
#include "permission.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[SEE-POLICY-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[SEE-POLICY-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void see_policy_test(void)
{
    failures = 0;
    serial_printf("[SEE-POLICY-TEST] === begin ===\n");

    see_policy_init();
    see_policy_init();
    check("policy_init idempotent", 1);

    uint32_t wl_count = 0;
    const struct see_wl_entry *wl = see_whitelist_default(&wl_count);
    check("whitelist_default count >= 5", wl != 0 && wl_count >= 5);

    int has_open = 0, has_load_driver = 0, has_create = 0;
    for (uint32_t i = 0; i < wl_count; ++i) {
        if (wl[i].syscall_nr == SYS_OB_OpenFile) has_open = 1;
        if (wl[i].syscall_nr == SYS_OB_LoadDriver) has_load_driver = 1;
        if (wl[i].syscall_nr == SYS_OB_CreateProcess) has_create = 1;
    }
    check("whitelist has SYS_OB_OpenFile", has_open);
    check("whitelist rejects SYS_OB_LoadDriver", !has_load_driver);
    check("whitelist rejects SYS_OB_CreateProcess", !has_create);

    struct see_instance inst;
    uint8_t *p = (uint8_t *)&inst;
    for (unsigned i = 0; i < sizeof(inst); ++i) p[i] = 0;
    see_policy_copy_default(&inst.policy);

    uint32_t added = 0;
    while (see_policy_add_rule(&inst, 0x200 + added, OB_RES_FILE, OB_ACCESS_READ) == 0) {
        added++;
        if (added > SEE_MAX_RULES + 4) break;
    }
    check("add_rule to limit", added == SEE_MAX_RULES);

    struct see_instance inst2;
    for (unsigned i = 0; i < sizeof(inst2); ++i) ((uint8_t *)&inst2)[i] = 0;
    see_policy_copy_default(&inst2.policy);
    see_whitelist_install_default(&inst2);

    check("policy_check hits open",
          see_policy_check(&inst2, SYS_OB_OpenFile) == 1);
    check("policy_check rejects load_driver",
          see_policy_check(&inst2, SYS_OB_LoadDriver) == 0);

    if (failures == 0) {
        serial_printf("[SEE-POLICY-TEST] selftest OK\n");
    } else {
        serial_printf("[SEE-POLICY-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[SEE-POLICY-TEST] === end ===\n");
}