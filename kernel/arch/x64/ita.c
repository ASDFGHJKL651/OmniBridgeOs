/*===OmniBridgeOs/kernel/arch/x64/ita.c===*/
#include "ita.h"
#include "ita_fixed_hashes.h"
#include "ita_manual.h"
#include "sha384.h"
#include "serial.h"
#include "audit.h"

#ifdef OB_QEMU_EXIT
extern void qemu_exit(uint32_t code);
#endif

static int path_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static int sha384_is_zero(const uint8_t h[48])
{
    for (int i = 0; i < 48; ++i) if (h[i] != 0) return 0;
    return 1;
}

void ita_init(void)
{
    if (g_ita_fixed_count == 0) {
        serial_printf("[ITA] FATAL: empty fixed whitelist\n");
        for (;;) { __asm__ __volatile__("hlt"); }
    }

    for (uint32_t i = 0; i < g_ita_fixed_count; ++i) {
        const struct ita_fixed_entry *e = &g_ita_fixed_whitelist[i];
        if (!e->path || e->path[0] == '\0') {
            serial_printf("[ITA] FATAL: entry %u has empty path\n",
                          (unsigned)i);
            for (;;) { __asm__ __volatile__("hlt"); }
        }
        if (sha384_is_zero(e->sha384)) {
            serial_printf("[ITA] WARN: entry %u ('%s') has all-zero hash "
                          "(placeholder)\n",
                          (unsigned)i, e->path);
        }
    }

    serial_printf("[ITA] init: %u fixed entries loaded\n",
                  (unsigned)g_ita_fixed_count);

    /* ★ 第 17 步：初始化手动白名单第三层 */
    ita_manual_init();
}

uint32_t ita_fixed_whitelist_count(void)
{
    return g_ita_fixed_count;
}

int ita_verify_fixed(const char *path, const void *buf, uint64_t size)
{
    if (!path) return -22; /* -EINVAL */

    const struct ita_fixed_entry *hit = 0;
    for (uint32_t i = 0; i < g_ita_fixed_count; ++i) {
        if (path_eq(g_ita_fixed_whitelist[i].path, path)) {
            hit = &g_ita_fixed_whitelist[i];
            break;
        }
    }
    if (!hit) return -2; /* -ENOENT */

    uint8_t digest[48];
    sha384(buf, size, digest);

    for (int i = 0; i < 48; ++i) {
        if (digest[i] != hit->sha384[i]) return -13; /* -EACCES */
    }
    return 0;
}

int ita_verify_all_critical(void)
{
    /*
     * 步骤 17 仍走占位路径：空 buffer 对 4 个核心路径逐一验证。
     * 人工必须审查：一旦步骤 11 实现 VFS，本函数必须改为读文件实际内容。
     */
    static const char *targets[] = {
        "/system/critical/init.obr",
        "/system/critical/secmgr.obr",
        "/system/critical/servicehost.obr",
        "/system/critical/auditd.obr",
        "/system/critical/kernel_manager.obr",
    };
    const int n = (int)(sizeof(targets) / sizeof(targets[0]));

    for (int i = 0; i < n; ++i) {
        int rc = ita_verify_fixed(targets[i], "", 0);
        if (rc == 0) {
            serial_printf("[ITA] OK: %s (placeholder verified)\n",
                          targets[i]);
        } else if (rc == -2) {
            serial_printf("[ITA] FATAL: %s missing from whitelist\n",
                          targets[i]);
            audit_ita_failure(targets[i], 0);
#ifdef OB_QEMU_EXIT
            qemu_exit(1);
#endif
            for (;;) { __asm__ __volatile__("hlt"); }
        } else {
            serial_printf("[ITA] FATAL: %s hash mismatch rc=%d\n",
                          targets[i], rc);
            audit_ita_failure(targets[i], 0);
#ifdef OB_QEMU_EXIT
            qemu_exit(1);
#endif
            for (;;) { __asm__ __volatile__("hlt"); }
        }
    }
    return 0;
}

/* ============================================================
 * ★ 第 17 步：第三层手动白名单
 * ============================================================ */

int ita_verify_manual(const char *path, const void *buf, uint64_t size)
{
    return ita_manual_verify(path, buf, size);
}

int ita_verify_any(const char *path, const void *buf, uint64_t size)
{
    if (!path) return -22; /* -EINVAL */

    int rc = ita_verify_fixed(path, buf, size);
    if (rc == -2) {
        /* 未命中第一层，回退到第三层手动白名单 */
        rc = ita_manual_verify(path, buf, size);
    }
    return rc;
}
/*===OmniBridgeOs/kernel/arch/x64/ita.c 结束===*/