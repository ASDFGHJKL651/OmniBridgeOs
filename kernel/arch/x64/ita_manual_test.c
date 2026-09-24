/*===OmniBridgeOs/kernel/arch/x64/ita_manual_test.c===*/
#include "ita_manual_test.h"
#include "ita_manual.h"
#include "sha384.h"
#include "task.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[ITA-MANUAL-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[ITA-MANUAL-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static void make_fake(struct task_t *t, uint64_t pid,
                      uint8_t priv, uint8_t ui, uint8_t sandbox)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;
    t->pid             = pid;
    t->privilege_level = priv;
    t->ui_token_valid  = ui;
    t->sandbox_flags   = sandbox;
    t->security_token.level = priv;
}

void ita_manual_test(void)
{
    failures = 0;
    serial_printf("[ITA-MANUAL-TEST] === begin ===\n");

    /* 1) init 幂等 */
    ita_manual_init();
    ita_manual_init();
    /* 先清空以确保初始状态为 0（防止之前测试残留） */
    ita_manual_clear();
    check("manual init idempotent", ita_manual_count() == 0);

    /* 2) 权限 9 + UI 添加成功 */
    struct task_t admin;
    make_fake(&admin, 9000, 9, 1, 0);

    uint8_t h1[48];
    sha384("hello-ita-manual", 15, h1);

    int rc = ita_manual_add(&admin, "/test/manual/a.obr", h1);
    check("add with priv9+ui succeeds",
          rc == 0 && ita_manual_count() == 1);

    /* 3) 权限 8 拒绝 */
    struct task_t p8;
    make_fake(&p8, 8000, 8, 1, 0);
    uint8_t h2[48];
    sha384("x", 1, h2);
    rc = ita_manual_add(&p8, "/test/manual/b.obr", h2);
    check("add with priv8 fails", rc == OB_EPERM);

    /* 4) 权限 9 无 UI 拒绝 */
    struct task_t p9_noui;
    make_fake(&p9_noui, 9001, 9, 0, 0);
    rc = ita_manual_add(&p9_noui, "/test/manual/c.obr", h2);
    check("add with no UI fails", rc == OB_EPERM);

    /* 5) 沙盒拒绝 */
    struct task_t sb;
    make_fake(&sb, 9002, 9, 1, 0x01);
    rc = ita_manual_add(&sb, "/test/manual/d.obr", h2);
    check("add from sandbox fails", rc == OB_EPERM);

    /* 6) verify 匹配 */
    const char *msg = "hello-ita-manual";
    rc = ita_manual_verify("/test/manual/a.obr", msg, 15);
    check("verify matches", rc == 0);

    /* 7) verify 不匹配 */
    rc = ita_manual_verify("/test/manual/a.obr", "tampered", 8);
    check("verify mismatch returns -EACCES", rc == OB_EACCES);

    /* 8) verify 路径不存在 */
    rc = ita_manual_verify("/test/manual/none.obr", msg, 15);
    check("verify unknown path returns -ENOENT", rc == OB_ENOENT);

    /* 9) contains */
    check("contains returns true",
          ita_manual_contains("/test/manual/a.obr") == 1);
    check("contains unknown returns false",
          ita_manual_contains("/test/manual/none.obr") == 0);

    /* 10) 更新已有条目 */
    uint8_t h3[48];
    sha384("updated", 7, h3);
    rc = ita_manual_add(&admin, "/test/manual/a.obr", h3);
    check("update existing entry",
          rc == 0 && ita_manual_count() == 1);
    rc = ita_manual_verify("/test/manual/a.obr", "updated", 7);
    check("verify after update", rc == 0);

    /* 11) clear */
    ita_manual_clear();
    check("clear resets count", ita_manual_count() == 0);

    if (failures == 0) {
        serial_printf("[ITA-MANUAL-TEST] selftest OK\n");
    } else {
        serial_printf("[ITA-MANUAL-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[ITA-MANUAL-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/ita_manual_test.c 结束===*/