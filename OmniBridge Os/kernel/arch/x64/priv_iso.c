/*===OmniBridgeOs/kernel/arch/x64/priv_iso.c===*/
#include "priv_iso.h"
#include "vfs.h"
#include "tmpfs.h"
#include "kmalloc.h"
#include "serial.h"
#include "rng.h"
#include "sha384.h"

static size_t ob_strlen_(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static int ct_mem_eq32(const uint8_t *a, const uint8_t *b)
{
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= a[i] ^ b[i];
    return acc == 0;
}

void priv_iso_path_hash(const char *path, uint8_t out_hash[48])
{
    if (!path || !out_hash) return;
    size_t len = ob_strlen_(path);
    sha384(path, (uint64_t)len, out_hash);
}

/* ============================================================
 * 权限 0：VFS-in-RAM
 * ============================================================ */

int priv0_mount_path(uint64_t pid, char *out, size_t out_size)
{
    if (!out || out_size < 16) return OB_EINVAL;

    int p = 0;
    const char *pfx = "/priv0/";
    while (pfx[p] && p < (int)out_size - 2) {
        out[p] = pfx[p];
        ++p;
    }

    char num[24];
    int nd = 0;
    uint64_t v = pid;
    if (v == 0) num[nd++] = '0';
    while (v && nd < (int)sizeof(num) - 1) {
        num[nd++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int k = nd - 1; k >= 0; --k) {
        if (p >= (int)out_size - 2) return OB_EINVAL;
        out[p++] = num[k];
    }
    if (p >= (int)out_size - 1) return OB_EINVAL;
    out[p] = '\0';
    return 0;
}

int priv0_mount_vfs(struct task_t *t)
{
    if (!t) return OB_EINVAL;
    if (t->priv0_sb_ptr) return 0;

    char mount_path[64];
    int rc = priv0_mount_path(t->pid, mount_path, sizeof(mount_path));
    if (rc != 0) return rc;

    rc = vfs_mkdir("/priv0", 0755);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[PRIV-ISO] mkdir /priv0 failed rc=%d\n", rc);
        return rc;
    }

    rc = vfs_mkdir(mount_path, 0755);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[PRIV-ISO] mkdir %s failed rc=%d\n", mount_path, rc);
        return rc;
    }

    struct vfs_superblock *sb = tmpfs_mount("priv0", 1024ULL * 1024ULL);
    if (!sb) return OB_ENOMEM;

    rc = vfs_mount(mount_path, sb);
    if (rc != 0) {
        tmpfs_umount(sb);
        serial_printf("[PRIV-ISO] mount %s failed rc=%d\n", mount_path, rc);
        return rc;
    }

    t->priv0_sb_ptr = sb;
    serial_printf("[PRIV-ISO] priv0 vfs mounted at %s (pid=%llu)\n",
                  mount_path, (unsigned long long)t->pid);
    return 0;
}

void priv0_umount_vfs(struct task_t *t)
{
    if (!t) return;
    if (!t->priv0_sb_ptr) return;

    char mount_path[64];
    if (priv0_mount_path(t->pid, mount_path, sizeof(mount_path)) == 0) {
        int rc = vfs_umount(mount_path);
        if (rc != 0) {
            serial_printf("[PRIV-ISO] WARN: umount %s rc=%d\n",
                          mount_path, rc);
        }
        rc = vfs_rmdir(mount_path);
        if (rc != 0 && rc != OB_ENOENT) {
            serial_printf("[PRIV-ISO] WARN: rmdir %s rc=%d\n",
                          mount_path, rc);
        }

        /* ★ 加固：尝试删除父目录 /priv0（若空）。
         *   多个进程可能共享 /priv0，因此非空时返回 -ENOTEMPTY 属于
         *   正常情况；此处仅做"尽力而为"的清理，避免长期运行过程中
         *   /priv0 目录本身泄漏一个 vfs_inode。 */
        if (vfs_rmdir("/priv0") == 0) {
            /* 成功删除，说明这是最后一个 priv0 进程；无需日志 */
        }
    }
    t->priv0_sb_ptr = 0;
}
/* ============================================================
 * 权限 1：临时专用目录
 * ============================================================ */

static void make_suffix(struct task_t *t, char out[9])
{
    /* ★ 显式清零，避免栈残留字节污染后缀 */
    for (int i = 0; i < 9; ++i) out[i] = '\0';

    uint8_t rnd[4] = {0, 0, 0, 0};
    if (rng_bytes(rnd, 4) == 0) {
        const char *alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        for (int i = 0; i < 8; ++i) {
            out[i] = alphabet[rnd[i & 3] % 32];
        }
    } else {
        serial_printf("[PRIV-ISO] WARN: rng_bytes failed, fallback suffix\n");
        uint32_t v = (uint32_t)(t->pid * 2654435761u);
        const char *hex = "0123456789abcdef";
        for (int i = 0; i < 8; ++i) {
            out[i] = hex[(v >> (i * 4)) & 0xF];
        }
    }
    out[8] = '\0';
}

int priv1_setup_tmpdir(struct task_t *t)
{
    serial_printf("[PRIV-ISO-DBG] setup_tmpdir: t=%p\n", (void *)t);
    if (!t) return OB_EINVAL;

    char suffix[9];
    serial_printf("[PRIV-ISO-DBG]   before make_suffix\n");
    make_suffix(t, suffix);
    serial_printf("[PRIV-ISO-DBG]   after  make_suffix suffix=%s\n", suffix);

    char path[128];
    int p = 0;
    const char *pfx = "/tmp/priv1_";
    while (pfx[p] && p < 100) { path[p] = pfx[p]; ++p; }

    char num[24];
    int nd = 0;
    uint64_t v = t->pid;
    if (v == 0) num[nd++] = '0';
    while (v && nd < (int)sizeof(num) - 1) {
        num[nd++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int k = nd - 1; k >= 0 && p < 110; --k) path[p++] = num[k];
    if (p < 111) path[p++] = '_';
    for (int i = 0; i < 8 && p < 120; ++i) path[p++] = suffix[i];
    path[p] = '\0';

    serial_printf("[PRIV-ISO-DBG]   path=%s\n", path);

    serial_printf("[PRIV-ISO-DBG]   before vfs_mkdir(/tmp)\n");
    int rc = vfs_mkdir("/tmp", 0777);
    serial_printf("[PRIV-ISO-DBG]   after  vfs_mkdir(/tmp) rc=%d\n", rc);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[PRIV-ISO] mkdir /tmp failed rc=%d\n", rc);
        return rc;
    }

    serial_printf("[PRIV-ISO-DBG]   before vfs_mkdir(%s)\n", path);
    rc = vfs_mkdir(path, 0700);
    serial_printf("[PRIV-ISO-DBG]   after  vfs_mkdir(%s) rc=%d\n", path, rc);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[PRIV-ISO] mkdir %s failed rc=%d\n", path, rc);
        return rc;
    }

    {
        int i = 0;
        while (path[i] && i < (int)sizeof(t->priv1_dir_path) - 1) {
            t->priv1_dir_path[i] = path[i];
            ++i;
        }
        t->priv1_dir_path[i] = '\0';
    }

    {
        char full[130];
        int j = 0;
        while (path[j] && j < (int)sizeof(full) - 2) {
            full[j] = path[j];
            ++j;
        }
        full[j++] = '/';
        full[j] = '\0';

        uint8_t h[48];
        priv_iso_path_hash(full, h);
        for (int k = 0; k < 32; ++k) {
            t->security_token.dir_whitelist_hash[k] = h[k];
        }
    }

    serial_printf("[PRIV-ISO] priv1 tmpdir created: %s (pid=%llu)\n",
                  path, (unsigned long long)t->pid);
    return 0;
}

void priv1_cleanup_tmpdir(struct task_t *t)
{
    if (!t || t->priv1_dir_path[0] == '\0') return;

    int rc = vfs_rmdir(t->priv1_dir_path);
    if (rc != 0 && rc != OB_ENOENT) {
        serial_printf("[PRIV-ISO] WARN: rmdir %s rc=%d\n",
                      t->priv1_dir_path, rc);
    }
    t->priv1_dir_path[0] = '\0';
}

int priv1_path_allowed(const struct task_t *t, const char *path)
{
    if (!t || !path) return 0;
    const char *prefix = t->priv1_dir_path;
    if (prefix[0] == '\0') return 0;

    int i = 0;
    while (prefix[i]) {
        if (path[i] != prefix[i]) return 0;
        ++i;
    }
    if (path[i] != '/' && path[i] != '\0') return 0;

    char full[130];
    int j = 0;
    while (prefix[j] && j < (int)sizeof(full) - 2) {
        full[j] = prefix[j];
        ++j;
    }
    if (j == 0 || full[j - 1] != '/') {
        if (j >= (int)sizeof(full) - 1) return 0;
        full[j++] = '/';
    }
    full[j] = '\0';

    uint8_t h[48];
    priv_iso_path_hash(full, h);
    return ct_mem_eq32(h, t->security_token.dir_whitelist_hash);
}
/*===OmniBridgeOs/kernel/arch/x64/priv_iso.c 结束===*/