/*===OmniBridgeOs/kernel/arch/x64/compat_handle_test.c===*/
#include "compat_handle_test.h"
#include "compat_handle.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[COMPAT-HANDLE-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[COMPAT-HANDLE-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void compat_handle_test(void)
{
    failures = 0;

    /* 1) 创建 */
    struct compat_handle_table *ht = compat_handle_table_create();
    check("create table", ht != 0);
    if (!ht) {
        serial_printf("[COMPAT-HANDLE-TEST] selftest FAILED\n");
        return;
    }
    check("initial count == 3", compat_handle_count(ht) == 3);

    /* 2) alloc */
    uint32_t h1 = compat_handle_alloc(ht, 0xDEADBEEFULL, OB_RES_FILE, 0);
    check("alloc returns >= 0x100", h1 >= COMPAT_HANDLE_BASE);
    check("count == 4 after alloc", compat_handle_count(ht) == 4);

    uint32_t h2 = compat_handle_alloc(ht, 0xCAFEBABEULL, OB_RES_MEMORY, 0x1);
    check("second alloc distinct", h2 != h1);
    check("count == 5 after second alloc", compat_handle_count(ht) == 5);

    /* 3) lookup */
    struct compat_handle_entry ent;
    int rc = compat_handle_lookup(ht, h1, &ent);
    check("lookup works",
          rc == 0 && ent.kernel_handle == 0xDEADBEEFULL &&
          ent.type == OB_RES_FILE && ent.in_use);

    rc = compat_handle_lookup(ht, h2, &ent);
    check("lookup second works",
          rc == 0 && ent.kernel_handle == 0xCAFEBABEULL &&
          ent.flags == 0x1);

    /* 4) 不存在的句柄 */
    rc = compat_handle_lookup(ht, 0x99999u, &ent);
    check("lookup unknown fails", rc != 0);

    /* 5) free */
    rc = compat_handle_free(ht, h1);
    check("free works", rc == 0 && compat_handle_count(ht) == 4);

    rc = compat_handle_lookup(ht, h1, &ent);
    check("lookup after free fails", rc != 0);

    /* 6) 重复 free 拒绝 */
    rc = compat_handle_free(ht, h1);
    check("double free rejected", rc != 0);

    /* 7) 0/1/2 预置 */
    rc = compat_handle_lookup(ht, 0, &ent);
    check("fd 0 preset", rc == 0);
    rc = compat_handle_lookup(ht, 1, &ent);
    check("fd 1 preset", rc == 0);
    rc = compat_handle_lookup(ht, 2, &ent);
    check("fd 2 preset", rc == 0);

    /* 8) 销毁 */
    compat_handle_table_destroy(ht);

    if (failures == 0) {
        serial_printf("[COMPAT-HANDLE-TEST] selftest OK\n");
    } else {
        serial_printf("[COMPAT-HANDLE-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}