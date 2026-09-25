#include "critical.h"
#include "vfs.h"
#include "tmpfs.h"
#include "serial.h"
#include "printk.h"
#include "audit.h"
#include "permission.h"   /* OB_ACCESS_* 定义 */

/* ---------- 状态 ---------- */
static int g_critical_inited = 0;

/* ---------- 字符串工具 ---------- */

static int str_match(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static int str_prefix(const char *s, const char *pfx)
{
    if (!s || !pfx) return 0;
    while (*pfx) {
        if (*s != *pfx) return 0;
        ++s; ++pfx;
    }
    return 1;
}

/* ---------- 路径判定 ---------- */

int path_is_critical_exact(const char *path)
{
    return str_match(path, CRITICAL_PATH);
}

int path_is_critical_path(const char *path)
{
    if (!path) return 0;
    if (!str_prefix(path, CRITICAL_PATH)) return 0;
    char next = path[CRITICAL_PATH_LEN];
    return (next == '\0' || next == '/');
}

/* ---------- 初始化 ---------- */

void critical_init(void)
{
    if (g_critical_inited) return;

    /* 1) 在根 tmpfs 上创建 /system 目录（已存在则忽略）。 */
    int rc = vfs_mkdir("/system", 0755);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[CRITICAL] FATAL: vfs_mkdir('/system') rc=%d\n", rc);
        return;
    }

    /* 2) 挂载独立 tmpfs 到 /system/critical。
     *    tmpfs 本身是读写的；"只读"语义由 critical_check_access()
     *    在权限检查层施加，与 §10.5.2 ACL 表一致。 */
    struct vfs_superblock *sb = tmpfs_mount("critical", 4ULL * 1024 * 1024);
    if (!sb) {
        serial_printf("[CRITICAL] FATAL: tmpfs_mount failed\n");
        return;
    }

    rc = vfs_mount(CRITICAL_PATH, sb);
    if (rc != 0) {
        serial_printf("[CRITICAL] FATAL: vfs_mount('%s') rc=%d\n",
                      CRITICAL_PATH, rc);
        tmpfs_umount(sb);
        return;
    }

    g_critical_inited = 1;
    serial_printf("[CRITICAL] mounted %s (read-only)\n", CRITICAL_PATH);
}

int critical_is_read_only(void)
{
    return 1;
}

/* ---------- ACL 决策 ---------- */

int critical_check_access(struct task_t *cur, const char *path,
                          uint32_t access_mode)
{
    if (!path) return 0;
    if (!path_is_critical_path(path)) return 0;

    /* 1) 内核引导阶段：只读允许；写/删拒绝。 */
    if (cur == 0) {
        if (access_mode & (OB_ACCESS_WRITE | OB_ACCESS_DELETE)) {
            return OB_EPERM;
        }
        return 0;
    }

    /* 2) 沙盒（最高优先级）：无条件拒绝。 */
    if (cur->sandbox_flags != 0) {
        audit_critical_access(cur->pid, path, access_mode);
        return OB_EPERM;
    }

    /* 3) PID 1..99 系统保留区：允许任何 mode。 */
    if (cur->pid >= 1 && cur->pid <= 99) {
        return 0;
    }

    /* 4) 权限 9 + 有效 UI 令牌：允许任何 mode。 */
    if (cur->privilege_level == 9 && cur->ui_token_valid) {
        return 0;
    }

    /* 5) 其他一切用户态进程（含权限 7/8、PID >= 100）：拒绝并审计。 */
    audit_critical_access(cur->pid, path, access_mode);
    return OB_EPERM;
}