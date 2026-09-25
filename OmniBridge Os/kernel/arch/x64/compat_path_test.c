/*===OmniBridgeOs/kernel/arch/x64/compat_path_test.c===*/
#include "compat_path_test.h"
#include "compat_path.h"
#include "task.h"
#include "serial.h"
#include "permission.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[COMPAT-PATH-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[COMPAT-PATH-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static int str_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static void make_fake(struct task_t *t, uint64_t pid, uint8_t priv,
                      uint8_t sandbox)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;
    t->pid             = pid;
    t->privilege_level = priv;
    t->sandbox_flags   = sandbox;
    t->security_token.level = priv;
}

void compat_path_test(void)
{
    failures = 0;
    serial_printf("[COMPAT-PATH-TEST] === begin ===\n");

    char buf[COMPAT_PATH_MAX];
    int rc;

    /* 1) Windows C:\Windows */
    rc = compat_path_win_to_ob("C:\\Windows\\System32", buf);
    check("win C:\\Windows -> /winmount/C/Windows",
          rc == 0 && str_eq(buf, "/winmount/C/Windows/System32"));

    /* 2) Windows 大小写不敏感 */
    rc = compat_path_win_to_ob("c:\\Temp\\x", buf);
    check("win c:\\Temp -> /winmount/C/Temp/x",
          rc == 0 && str_eq(buf, "/winmount/C/Temp/x"));

    /* 3) HKLM */
    rc = compat_path_win_to_ob("HKLM\\SOFTWARE\\Foo", buf);
    check("win HKLM -> /system/registry/HKLM",
          rc == 0 && str_eq(buf, "/system/registry/HKLM/SOFTWARE/Foo"));

    /* 4) HKCU */
    rc = compat_path_win_to_ob("HKCU\\Software", buf);
    check("win HKCU -> /system/registry/HKCU/0",
          rc == 0 && str_eq(buf, "/system/registry/HKCU/0/Software"));

    /* 5) Windows .. 逃逸拒绝 */
    rc = compat_path_win_to_ob("C:\\..\\..\\kernel", buf);
    check("win no .. escape", rc != 0);

    /* 6) Linux /tmp -> /tmp/linux_temp */
    rc = compat_path_linux_to_ob("/tmp/foo", buf);
    check("linux /tmp -> /tmp/linux_temp",
          rc == 0 && str_eq(buf, "/tmp/linux_temp/foo"));

    /* 7) Linux /proc */
    rc = compat_path_linux_to_ob("/proc/1/status", buf);
    check("linux /proc -> /vfs/virtual/proc",
          rc == 0 && str_eq(buf, "/vfs/virtual/proc/1/status"));

    /* 8) Linux /sys */
    rc = compat_path_linux_to_ob("/sys/devices/x", buf);
    check("linux /sys -> /vfs/virtual/sys",
          rc == 0 && str_eq(buf, "/vfs/virtual/sys/devices/x"));

    /* 9) Linux /dev */
    rc = compat_path_linux_to_ob("/dev/null", buf);
    check("linux /dev -> /vfs/virtual/dev",
          rc == 0 && str_eq(buf, "/vfs/virtual/dev/null"));

    /* 10) Linux /home */
    rc = compat_path_linux_to_ob("/home/user/file", buf);
    check("linux /home -> /home/linux/0",
          rc == 0 && str_eq(buf, "/home/linux/0/user/file"));

    /* 11) Linux .. 逃逸拒绝 */
    rc = compat_path_linux_to_ob("/tmp/../../../kernel", buf);
    check("linux no .. escape", rc != 0);

    /* 12) compat_path_check：/kernel/foo 拒绝 */
    struct task_t fake;
    make_fake(&fake, 5000, 8, 0);
    rc = compat_path_check("/kernel/foo", &fake, OB_ACCESS_READ);
    check("/kernel/foo rejected", rc == OB_EPERM);

    /* 13) /system/kernel/bar 拒绝 */
    rc = compat_path_check("/system/kernel/bar", &fake, OB_ACCESS_READ);
    check("/system/kernel/bar rejected", rc == OB_EPERM);

    /* 14) /system/critical/x 对 priv8 拒绝 */
    rc = compat_path_check("/system/critical/x", &fake, OB_ACCESS_READ);
    check("/system/critical/x denied for priv8", rc == OB_EPERM);

    /* 15) /system/critical/x 对 pid=5（保留区）允许 */
    make_fake(&fake, 5, 8, 0);
    rc = compat_path_check("/system/critical/x", &fake, OB_ACCESS_READ);
    check("/system/critical/x allowed for pid5", rc == 0);

    /* 16) 正常路径允许 */
    make_fake(&fake, 5000, 8, 0);
    rc = compat_path_check("/winmount/C/Windows", &fake, OB_ACCESS_READ);
    check("normal path allowed", rc == 0);

    if (failures == 0) {
        serial_printf("[COMPAT-PATH-TEST] selftest OK\n");
    } else {
        serial_printf("[COMPAT-PATH-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[COMPAT-PATH-TEST] === end ===\n");
}