/* kernel/arch/x64/compat_path.c */
#include "compat_path.h"
#include "permission.h"
#include "critical.h"
#include "audit.h"
#include "serial.h"

/* ---------- 本地工具 ---------- */

static size_t ob_strlen_(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

/* 大小写不敏感比较前 n 个字符。返回 1 相等。 */
static int ci_eq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

/* 检测路径中是否含 ".." 分量。返回 1 表示含有。 */
static int has_dotdot(const char *path)
{
    for (size_t i = 0; path[i]; ++i) {
        if (path[i] != '.') continue;
        if (path[i + 1] != '.') continue;
        int ok_left  = (i == 0) || (path[i - 1] == '/') ||
                       (path[i - 1] == '\\');
        int ok_right = (path[i + 2] == '\0') ||
                       (path[i + 2] == '/') || (path[i + 2] == '\\');
        if (ok_left && ok_right) return 1;
    }
    return 0;
}

/* 追加字符串到 out（已保证容量）。 */
static int append_str(char *out, size_t cap, size_t *pos, const char *s)
{
    while (s && *s) {
        if (*pos + 1 >= cap) return -1;
        out[(*pos)++] = *s++;
    }
    out[*pos] = '\0';
    return 0;
}

/* 追加路径分量（把 '\\' 替换为 '/'）。 */
static int append_path_body(char *out, size_t cap, size_t *pos,
                            const char *s)
{
    while (s && *s) {
        char c = *s++;
        if (c == '\\') c = '/';
        if (*pos + 1 >= cap) return -1;
        out[(*pos)++] = c;
    }
    out[*pos] = '\0';
    return 0;
}

/* ---------- Windows 路径 ---------- */

int compat_path_win_to_ob(const char *win_path, char out[COMPAT_PATH_MAX])
{
    if (!win_path || !out) return OB_EINVAL;

    size_t len = ob_strlen_(win_path);
    if (len == 0) return OB_EINVAL;
    if (len >= COMPAT_PATH_MAX * 2) return OB_EINVAL;

    out[0] = '\0';
    size_t pos = 0;

    /* 环境变量展开 */
    if (ci_eq_n(win_path, "%WINDIR%", 8)) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/winmount/Windows") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             win_path + 8) != 0) return OB_EINVAL;
        goto check;
    }
    if (ci_eq_n(win_path, "%TEMP%", 6)) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/tmp/win_temp") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             win_path + 6) != 0) return OB_EINVAL;
        goto check;
    }

    /* 注册表 HKLM / HKCU */
    if (ci_eq_n(win_path, "HKLM", 4) &&
        (win_path[4] == '\\' || win_path[4] == '/')) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/system/registry/HKLM") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             win_path + 4) != 0) return OB_EINVAL;
        goto check;
    }
    if (ci_eq_n(win_path, "HKCU", 4) &&
        (win_path[4] == '\\' || win_path[4] == '/')) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/system/registry/HKCU/0") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             win_path + 4) != 0) return OB_EINVAL;
        goto check;
    }

    /* 盘符 X:\... */
    if (len >= 3 && win_path[1] == ':' &&
        (win_path[2] == '\\' || win_path[2] == '/')) {
        char drive = win_path[0];
        if (drive >= 'a' && drive <= 'z') drive = (char)(drive - 'a' + 'A');
        if (!(drive >= 'A' && drive <= 'Z')) return OB_EINVAL;

        if (pos + 1 >= COMPAT_PATH_MAX) return OB_EINVAL;
        out[pos++] = '/';
        out[pos++] = 'w';
        out[pos++] = 'i';
        out[pos++] = 'n';
        out[pos++] = 'm';
        out[pos++] = 'o';
        out[pos++] = 'u';
        out[pos++] = 'n';
        out[pos++] = 't';
        out[pos++] = '/';
        out[pos++] = drive;
        out[pos]   = '\0';

        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             win_path + 2) != 0) return OB_EINVAL;
        goto check;
    }

    /* 相对路径：直接按 POSIX 处理（拒绝逃逸） */
    if (append_path_body(out, COMPAT_PATH_MAX, &pos, win_path) != 0) {
        return OB_EINVAL;
    }

check:
    if (has_dotdot(out)) {
        out[0] = '\0';
        return OB_EINVAL;
    }
    return 0;
}

/* ---------- Linux 路径 ---------- */

int compat_path_linux_to_ob(const char *linux_path, char out[COMPAT_PATH_MAX])
{
    if (!linux_path || !out) return OB_EINVAL;

    size_t len = ob_strlen_(linux_path);
    if (len == 0) return OB_EINVAL;
    if (len >= COMPAT_PATH_MAX * 2) return OB_EINVAL;

    out[0] = '\0';
    size_t pos = 0;

    /* /proc/... -> /vfs/virtual/proc/... */
    if (len >= 5 && linux_path[0] == '/' &&
        linux_path[1] == 'p' && linux_path[2] == 'r' &&
        linux_path[3] == 'o' && linux_path[4] == 'c' &&
        (len == 5 || linux_path[5] == '/')) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/vfs/virtual/proc") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             linux_path + 5) != 0) return OB_EINVAL;
        goto check;
    }

    /* /sys/... -> /vfs/virtual/sys/... */
    if (len >= 4 && linux_path[0] == '/' &&
        linux_path[1] == 's' && linux_path[2] == 'y' &&
        linux_path[3] == 's' &&
        (len == 4 || linux_path[4] == '/')) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/vfs/virtual/sys") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             linux_path + 4) != 0) return OB_EINVAL;
        goto check;
    }

    /* /dev/... -> /vfs/virtual/dev/... */
    if (len >= 4 && linux_path[0] == '/' &&
        linux_path[1] == 'd' && linux_path[2] == 'e' &&
        linux_path[3] == 'v' &&
        (len == 4 || linux_path[4] == '/')) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/vfs/virtual/dev") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             linux_path + 4) != 0) return OB_EINVAL;
        goto check;
    }

    /* /tmp 或 /tmp/... -> /tmp/linux_temp[/...] */
    if (len >= 4 && linux_path[0] == '/' &&
        linux_path[1] == 't' && linux_path[2] == 'm' &&
        linux_path[3] == 'p' &&
        (len == 4 || linux_path[4] == '/')) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/tmp/linux_temp") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             linux_path + 4) != 0) return OB_EINVAL;
        goto check;
    }

    /* /home/... -> /home/linux/0/... */
    if (len >= 5 && linux_path[0] == '/' &&
        linux_path[1] == 'h' && linux_path[2] == 'o' &&
        linux_path[3] == 'm' && linux_path[4] == 'e' &&
        (len == 5 || linux_path[5] == '/')) {
        if (append_str(out, COMPAT_PATH_MAX, &pos,
                       "/home/linux/0") != 0) return OB_EINVAL;
        if (append_path_body(out, COMPAT_PATH_MAX, &pos,
                             linux_path + 5) != 0) return OB_EINVAL;
        goto check;
    }

    /* 其他路径：原样拷贝 */
    if (append_path_body(out, COMPAT_PATH_MAX, &pos, linux_path) != 0) {
        return OB_EINVAL;
    }

check:
    if (has_dotdot(out)) {
        out[0] = '\0';
        return OB_EINVAL;
    }
    return 0;
}

/* ---------- 统一安全检查 ---------- */

int compat_path_check(const char *ob_path, struct task_t *cur,
                      uint32_t access_mode)
{
    if (!ob_path) return OB_EINVAL;
    if (ob_path[0] == '\0') return OB_EINVAL;

    /* 1) 内核绝对禁区 */
    if (path_is_kernel_protected(ob_path)) {
        audit_event(AUDIT_EV_COMPAT_PATH_REDIRECT, AUDIT_LVL_CRITICAL,
                    cur ? cur->pid : 0, 0, 0, access_mode, ob_path);
        return OB_EPERM;
    }

    /* 2) /system/critical/ 走 ACL */
    if (path_is_critical_path(ob_path)) {
        int rc = critical_check_access(cur, ob_path, access_mode);
        if (rc != 0) {
            audit_event(AUDIT_EV_COMPAT_PATH_REDIRECT, AUDIT_LVL_CRITICAL,
                        cur ? cur->pid : 0, 0, 0, access_mode, ob_path);
        }
        return rc;
    }

    return 0;
}