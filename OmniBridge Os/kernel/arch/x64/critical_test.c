#include "critical_test.h"
#include "critical.h"
#include "task.h"
#include "vfs.h"
#include "permission.h"
#include "serial.h"

/*
 * 第 12 步 /system/critical/ ACL 自检（表格驱动）。
 * 使用纯栈上 fake task，严禁 task_create。
 */

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[CRITICAL-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[CRITICAL-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static void make_fake(struct task_t *t, uint64_t pid, uint8_t priv,
                      uint8_t sandbox, uint8_t ui_token)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;
    t->pid                  = pid;
    t->privilege_level      = priv;
    t->sandbox_flags        = sandbox;
    t->ui_token_valid       = ui_token;
    t->security_token.level = priv;
}

void critical_test(void)
{
    failures = 0;

    serial_printf("[CRITICAL-TEST] === begin ===\n");

    /* 1) critical_init 幂等 */
    uint32_t m1 = vfs_mount_count();
    critical_init();
    uint32_t m2 = vfs_mount_count();
    check("critical_init idempotent", m1 == m2);

    /* 2) 路径判定 */
    check("exact('/system/critical')",
          path_is_critical_exact("/system/critical") == 1);
    check("exact('/system/critical/x') == 0",
          path_is_critical_exact("/system/critical/x") == 0);
    check("path('/system/critical')",
          path_is_critical_path("/system/critical") == 1);
    check("path('/system/critical/')",
          path_is_critical_path("/system/critical/") == 1);
    check("path('/system/critical/x')",
          path_is_critical_path("/system/critical/x") == 1);
    check("path('/system/criticalX') == 0",
          path_is_critical_path("/system/criticalX") == 0);
    check("path('/kernel/foo') == 0",
          path_is_critical_path("/kernel/foo") == 0);
    check("path(NULL) == 0",
          path_is_critical_path(0) == 0);

    /* 3) ACL 表驱动 */
    struct task_t fake;
    int rc;

    /* cur == NULL */
    rc = critical_check_access(0, "/system/critical/x", OB_ACCESS_READ);
    check("NULL + READ -> 0", rc == 0);
    rc = critical_check_access(0, "/system/critical/x", OB_ACCESS_WRITE);
    check("NULL + WRITE -> -EPERM", rc == OB_EPERM);
    rc = critical_check_access(0, "/system/critical/x", OB_ACCESS_DELETE);
    check("NULL + DELETE -> -EPERM", rc == OB_EPERM);

    /* pid=1，priv=8：允许任何 */
    make_fake(&fake, 1, 8, 0, 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_READ);
    check("pid=1 READ -> 0", rc == 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_WRITE);
    check("pid=1 WRITE -> 0", rc == 0);

    /* pid=50，priv=7：PID 1-99 保留区，允许 */
    make_fake(&fake, 50, 7, 0, 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_WRITE);
    check("pid=50 WRITE -> 0", rc == 0);

    /* pid=2000，priv=9 + UI=1：允许 */
    make_fake(&fake, 2000, 9, 0, 1);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_WRITE);
    check("pid=2000 priv9+UI WRITE -> 0", rc == 0);

    /* pid=2000，priv=9 无 UI：拒绝 */
    make_fake(&fake, 2000, 9, 0, 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_WRITE);
    check("pid=2000 priv9 no UI WRITE -> -EPERM", rc == OB_EPERM);

    /* pid=2000，priv=8：拒绝 */
    make_fake(&fake, 2000, 8, 0, 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_READ);
    check("pid=2000 priv8 READ -> -EPERM", rc == OB_EPERM);

    /* pid=2000，priv=7：拒绝 */
    make_fake(&fake, 2000, 7, 0, 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_READ);
    check("pid=2000 priv7 READ -> -EPERM", rc == OB_EPERM);

    /* pid=2000，沙盒：拒绝 */
    make_fake(&fake, 2000, 8, 1, 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_READ);
    check("pid=2000 sandbox READ -> -EPERM", rc == OB_EPERM);

    /* pid=50 + 沙盒：沙盒优先级最高 */
    make_fake(&fake, 50, 8, 1, 0);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_READ);
    check("pid=50 sandbox READ -> -EPERM", rc == OB_EPERM);

    /* pid=2000，priv=9，UI=1 + 沙盒：沙盒优先级最高 */
    make_fake(&fake, 2000, 9, 1, 1);
    rc = critical_check_access(&fake, "/system/critical/x", OB_ACCESS_WRITE);
    check("pid=2000 priv9+UI sandbox WRITE -> -EPERM", rc == OB_EPERM);

    /* 非 critical 路径不拦截 */
    make_fake(&fake, 2000, 5, 0, 0);
    rc = critical_check_access(&fake, "/kernel/foo", OB_ACCESS_READ);
    check("non-critical path returns 0", rc == 0);

    /* 4) 端到端：通过 permission_check_file */
    make_fake(&fake, 2000, 8, 0, 0);
    rc = permission_check_file(&fake, "/system/critical/x", OB_ACCESS_READ);
    check("permission_check_file priv8 read -> -EPERM", rc == OB_EPERM);

    make_fake(&fake, 5, 8, 0, 0);
    rc = permission_check_file(&fake, "/system/critical/x", OB_ACCESS_WRITE);
    check("permission_check_file pid5 write -> 0", rc == 0);

    if (failures == 0) {
        serial_printf("[CRITICAL-TEST] selftest OK\n");
    } else {
        serial_printf("[CRITICAL-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[CRITICAL-TEST] === end ===\n");
}