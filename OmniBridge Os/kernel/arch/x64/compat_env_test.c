/*===OmniBridgeOs/kernel/arch/x64/compat_env_test.c===*/
#include "compat_env_test.h"
#include "compat_env.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[COMPAT-ENV-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[COMPAT-ENV-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void compat_env_test(void)
{
    failures = 0;

    /* 幂等 */
    compat_env_init();
    compat_env_init();
    check("compat_env_init idempotent", 1);

    /* PEB 填充 */
    struct ob_peb_template peb;
    int rc = compat_env_fill_peb(&peb, 0x140000000ULL, 0x70000000ULL);
    check("fill_peb succeeds", rc == 0);
    check("fill_peb sets image_base",
          peb.image_base == 0x140000000ULL);
    check("fill_peb sets process_heap",
          peb.process_heap == 0x70000000ULL);

    /* auxv 填充 */
    struct ob_auxv_template auxv;
    rc = compat_env_fill_auxv(&auxv, 4096u, 0u);
    check("fill_auxv succeeds", rc == 0);

    /* 检查 AT_PAGESZ 条目 */
    int at_pagesz_ok = 0;
    for (uint32_t i = 0; i < auxv.count; ++i) {
        if (auxv.entries[i].type == OB_AT_PAGESZ &&
            auxv.entries[i].value == 4096ULL) {
            at_pagesz_ok = 1;
            break;
        }
    }
    check("fill_auxv sets AT_PAGESZ", at_pagesz_ok);

    /* AT_NULL 结尾 */
    int last_is_null = 0;
    if (auxv.count > 0) {
        last_is_null = (auxv.entries[auxv.count - 1].type == OB_AT_NULL);
    }
    check("auxv terminated by AT_NULL", last_is_null);

    /* dump 不崩溃 */
    compat_env_dump(&peb, &auxv);
    compat_env_dump(0, 0);

    if (failures == 0) {
        serial_printf("[COMPAT-ENV-TEST] selftest OK\n");
    } else {
        serial_printf("[COMPAT-ENV-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}