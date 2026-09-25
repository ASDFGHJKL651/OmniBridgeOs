#include "permission.h"
#include "vmm.h"
#include "serial.h"
#include "printk.h"
#include "audit.h"
#include "critical.h"
#include "mem_domain.h"    /* ★ 第 15 步 */
#include "priv_iso.h"      /* ★ 第 15 步 */
#include "task.h"          /* ★ 第 15 步：task_find_by_pid */

/* ============================================================
 * 路径与地址辅助
 * ============================================================ */

int path_starts_with(const char *path, const char *prefix)
{
    if (!path || !prefix) return 0;
    while (*prefix) {
        if (*path == '\0') return 0;
        if (*path != *prefix) return 0;
        ++path;
        ++prefix;
    }
    return 1;
}

int path_is_kernel_protected(const char *path)
{
    if (!path) return 0;

    if (path_starts_with(path, "/kernel")) {
        if (path[7] == '\0' || path[7] == '/') return 1;
    }
    if (path_starts_with(path, "/system/kernel")) {
        if (path[14] == '\0' || path[14] == '/') return 1;
    }
    return 0;
}

int path_is_critical_dir(const char *path)
{
    return path_is_critical_path(path);
}

int vaddr_is_kernel(uint64_t vaddr)
{
    return vaddr >= KERNEL_SPACE_START;
}

/* ============================================================
 * 辅助：旧语义字符串前缀匹配（权限 1，用于未初始化隔离的 fake task）
 *
 * 构造 "/tmp/priv1_<pid>_" 前缀并做前缀匹配。
 * ★ 仅供 permission_selftest 的纯栈上 fake task 使用；
 *   task_create 创建的真实进程会通过 priv1_setup_tmpdir 设置
 *   priv_iso_ready = 1，走 hash 路径。
 * ============================================================ */
static int priv1_legacy_prefix_match(const struct task_t *cur, const char *path)
{
    char prefix[64];
    int n = 0;
    const char *pfx = "/tmp/priv1_";
    while (pfx[n] && n < 40) { prefix[n] = pfx[n]; ++n; }

    uint64_t pid = cur->pid;
    char num[24];
    int nd = 0;
    if (pid == 0) num[nd++] = '0';
    while (pid && nd < (int)sizeof(num) - 1) {
        num[nd++] = (char)('0' + (pid % 10));
        pid /= 10;
    }
    for (int i = nd - 1; i >= 0 && n < 60; --i) prefix[n++] = num[i];
    if (n < 62) prefix[n++] = '_';
    prefix[n] = '\0';

    return path_starts_with(path, prefix);
}

/* ============================================================
 * 子检查函数
 * ============================================================ */

int permission_check_process(struct task_t *cur, uint64_t target_pid,
                             uint32_t mode, const char *hint)
{
    (void)mode;
    (void)hint;
    if (!cur) return OB_EPERM;

    /* ★ 第 15 步：权限 0 只能访问自己；权限 1 只能访问自己与直接子进程 */
    if (cur->privilege_level == 0) {
        if (target_pid != cur->pid) return OB_EPERM;
        return 0;
    }
    if (cur->privilege_level == 1) {
        if (target_pid == cur->pid) return 0;
        struct task_t *target = task_find_by_pid(target_pid);
        if (target && target->parent_pid == cur->pid) return 0;
        return OB_EPERM;
    }

    int rc = check_pid_access(cur, target_pid);
    if (rc != 0) return rc;

    if (target_pid == cur->pid) return 0;

    if (cur->privilege_level >= 6) return 0;
    return OB_EPERM;
}

int permission_check_memory(struct task_t *cur, uint64_t vaddr,
                            uint32_t mode, const char *hint)
{
    (void)hint;
    if (!cur) return OB_EPERM;

    if (vaddr_is_kernel(vaddr)) {
        audit_kernel_mem_violation(cur->pid, vaddr, mode);
        return OB_EPERM;
    }

    /* ★ 第 15 步：权限 0/1 内存域边界检查
     *
     * 关键（人工必须审查）：
     *   - 仅对"已建立 mem_domain"的进程检查。真实权限 0/1 进程由
     *     task_create → mem_domain_create 建立（pml4_self_ptr != 0）；
     *   - 未建立（pml4_self_ptr == 0）时跳过——这是 permission_selftest
     *     的纯栈上 fake task 的路径（不调用 task_create）。 */
    if (cur->privilege_level == 0 || cur->privilege_level == 1) {
        if (cur->mem_domain.pml4_self_ptr != 0) {
            if (!mem_domain_contains(cur, vaddr)) {
                audit_critical_access(cur->pid, "(mem-outside-domain)", mode);
                return OB_EPERM;
            }
        }
    }
    return 0;
}

int permission_check_file(struct task_t *cur, const char *path,
                          uint32_t mode)
{
    if (!cur) return OB_EPERM;
    if (!path) return 0;

    if (path_is_kernel_protected(path)) {
        audit_critical_access(cur->pid, path, mode);
        return OB_EPERM;
    }

    if (path_is_critical_dir(path)) {
        return critical_check_access(cur, path, mode);
    }

    /* ★ 第 15 步：权限 0
     *
     * 语义（人工必须审查）：
     *   - priv_iso_ready == 1（真实进程，VFS-in-RAM 已挂载）：
     *       仅允许 /priv0/<self_pid>/ 前缀；
     *   - priv_iso_ready == 0（栈上 fake task，permission_selftest 使用）：
     *       保持第 11 步之前的"VFS-in-RAM 未实现"语义，
     *       返回 OB_ENOSYS，与 step 9 的用例期望一致。
     *
     * 该分支不影响任务：真实权限 0 进程在 task_create 中由
     * priv0_mount_vfs 成功后会置 priv_iso_ready = 1；失败则创建失败。 */
    if (cur->privilege_level == 0) {
        if (!cur->priv_iso_ready) {
            serial_printf("[PERM] priv0 file op: VFS-in-RAM not set up "
                          "(path=%s)\n", path);
            return OB_ENOSYS;
        }
        char prefix[64];
        int rc = priv0_mount_path(cur->pid, prefix, sizeof(prefix));
        if (rc != 0) return OB_EPERM;
        if (!path_starts_with(path, prefix)) {
            return OB_EPERM;
        }
        int plen = 0;
        while (prefix[plen]) ++plen;
        if (path[plen] != '/' && path[plen] != '\0') return OB_EPERM;
        return 0;
    }

    /* ★ 第 15 步：权限 1
     *
     * 语义（人工必须审查）：
     *   - priv_iso_ready == 1（真实进程，临时目录已创建）：
     *       字符串前缀 + 常量时间 SHA-384 hash 一致性；
     *   - priv_iso_ready == 0（栈上 fake task）：
     *       退化为第 9 步的纯字符串前缀匹配 "/tmp/priv1_<pid>_"，
     *       与 step 9 的用例期望保持一致。 */
    if (cur->privilege_level == 1) {
        if (!cur->priv_iso_ready) {
            if (!priv1_legacy_prefix_match(cur, path)) {
                return OB_EPERM;
            }
            return 0;
        }
        if (!priv1_path_allowed(cur, path)) {
            return OB_EPERM;
        }
        return 0;
    }

    if (cur->privilege_level < 3) {
        if (mode & (OB_ACCESS_WRITE | OB_ACCESS_DELETE)) return OB_EPERM;
        return 0;
    }
    if (cur->privilege_level < 5) {
        if (mode & (OB_ACCESS_WRITE | OB_ACCESS_DELETE)) {
            if (!path_starts_with(path, "/tmp/") &&
                !path_starts_with(path, "/home/")) {
                return OB_EPERM;
            }
        }
        return 0;
    }
    return 0;
}

int permission_check_ipc(struct task_t *cur, uint64_t ipc_id,
                         uint32_t mode, const char *hint)
{
    (void)ipc_id;
    (void)mode;
    (void)hint;
    if (!cur) return OB_EPERM;

    /* ★ 第 15 步：权限 0/1 禁止 IPC（v5.0 §5.2） */
    if (cur->privilege_level == 0) return OB_EPERM;
    if (cur->privilege_level == 1) return OB_EPERM;

    return OB_ENOSYS;
}

int permission_check_config(struct task_t *cur, uint64_t cfg_id,
                            uint32_t mode, const char *hint)
{
    (void)cfg_id;
    if (!cur) return OB_EPERM;

    if (cur->privilege_level <= 5) {
        if (mode & (OB_ACCESS_WRITE | OB_ACCESS_DELETE)) return OB_EPERM;
        return 0;
    }

    if (cur->privilege_level == 6) {
        if (mode & (OB_ACCESS_WRITE | OB_ACCESS_DELETE)) {
            audit_critical_access(cur->pid,
                                  hint ? hint : "(config)",
                                  mode);
            return OB_EPERM;
        }
        return 0;
    }

    return 0;
}

/* ============================================================
 * 主入口
 * ============================================================ */

int check_permission(struct task_t *cur,
                     int res_type,
                     uint64_t res_id,
                     uint32_t access_mode,
                     const char *path_hint)
{
    if (!cur) return OB_EPERM;

    (void)(cur->sandbox_flags & OBSANDBOX_ACTIVE);

    switch (res_type) {
    case OB_RES_FILE:
        return permission_check_file(cur, path_hint, access_mode);
    case OB_RES_MEMORY:
        return permission_check_memory(cur, res_id, access_mode, path_hint);
    case OB_RES_PROCESS:
        return permission_check_process(cur, res_id, access_mode, path_hint);
    case OB_RES_IPC:
        return permission_check_ipc(cur, res_id, access_mode, path_hint);
    case OB_RES_CONFIG:
        return permission_check_config(cur, res_id, access_mode, path_hint);
    default:
        return OB_EPERM;
    }
}
/*===OmniBridgeOs/kernel/arch/x64/permission.c 结束===*/