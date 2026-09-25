#include "permission_test.h"
#include "permission.h"
#include "task.h"
#include "vmm.h"
#include "serial.h"

/*
 * 权限引擎自检（第 9 步）。
 *
 * 打印约定：
 *   [PERM] OK  : <desc>
 *   [PERM] FAIL: <desc> expected=<0|-1> got=<rc>
 * 末尾：
 *   [PERM] selftest OK (<n> cases)
 *   或
 *   [PERM] selftest FAILED: <failed>/<total> case(s)
 *
 * 严禁调用 task_create / kmalloc / pmm_alloc_pages —— 见 permission_test.h。
 *
 * 关于路径前缀判定的语义（与本文件用例保持一致，人工必须审查）：
 *   - "/kernel"           → 命中（目录自身）
 *   - "/kernel/"          → 命中
 *   - "/kernelX"          → 不命中（名字完全不同，规格未要求）
 *   - "/system/kernel"    → 命中（目录自身）
 *   - "/system/kernel/"   → 命中
 *   - "/system/kernelX"   → 不命中
 *   - "/system/critical"  → 命中
 *   - "/system/critical/" → 命中
 *   - "/system/criticalX" → 不命中
 * 判定实现位于 permission.c:path_is_kernel_protected / path_is_critical_dir。
 */

struct perm_test_case {
    uint8_t  caller_priv;
    uint8_t  caller_is_critical;
    uint8_t  caller_sandbox;
    uint8_t  caller_ui_token;
    uint64_t caller_pid;
    int      res_type;
    uint64_t res_id;
    uint32_t access_mode;
    const char *path_hint;
    int      expected;      /* 0 = 允许；非 0 = 拒绝 */
    const char *desc;
};

/* 用于测试的常量地址 */
#define K_ADDR      0xFFFF800000000000ULL   /* DirectMap 起点，内核空间 */
#define K_ADDR_HI   0xFFFFFFFF80000000ULL   /* 内核镜像起点 */
#define U_ADDR      0x0000000000400000ULL   /* 典型用户地址 */

static const struct perm_test_case g_cases[] = {

    /* ============================================================
     * MEMORY
     * ============================================================ */

    /* 内核地址：一律拒绝（例外只能走 OB_ReadKernelMemory /
     * OB_WriteKernelMemory 专用调用）。 */
    { 8, 0, 0, 0, 5000, OB_RES_MEMORY, K_ADDR,    OB_ACCESS_READ,  NULL, -1, "priv8 read kernel mem" },
    { 8, 0, 0, 0, 5000, OB_RES_MEMORY, K_ADDR,    OB_ACCESS_WRITE, NULL, -1, "priv8 write kernel mem" },
    { 9, 0, 0, 0, 5000, OB_RES_MEMORY, K_ADDR,    OB_ACCESS_READ,  NULL, -1, "priv9 no-ui read kernel mem" },
    { 9, 0, 0, 0, 5000, OB_RES_MEMORY, K_ADDR,    OB_ACCESS_WRITE, NULL, -1, "priv9 no-ui write kernel mem" },
    { 9, 0, 0, 1, 5000, OB_RES_MEMORY, K_ADDR,    OB_ACCESS_READ,  NULL, -1, "priv9+ui read via plain MEMORY (denied; use OB_ReadKernelMemory)" },
    { 0, 0, 0, 0, 5000, OB_RES_MEMORY, K_ADDR,    OB_ACCESS_READ,  NULL, -1, "priv0 read kernel mem" },
    { 9, 0, 0, 1, 5000, OB_RES_MEMORY, K_ADDR_HI, OB_ACCESS_WRITE, NULL, -1, "priv9+ui write kernel image via plain MEMORY (denied)" },

    /* 用户地址（选项 A 放行：mem_domain 判定留待步骤 15） */
    { 0, 0, 0, 0, 5000, OB_RES_MEMORY, U_ADDR, OB_ACCESS_READ,  NULL, 0, "priv0 read user mem" },
    { 0, 0, 0, 0, 5000, OB_RES_MEMORY, U_ADDR, OB_ACCESS_WRITE, NULL, 0, "priv0 write user mem" },
    { 5, 0, 0, 0, 5000, OB_RES_MEMORY, U_ADDR, OB_ACCESS_WRITE, NULL, 0, "priv5 write user mem" },

    /* ============================================================
     * FILE：/kernel 与 /system/kernel 无条件拒绝
     * ============================================================ */

    { 0, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/kernel/foo",           -1, "priv0 read /kernel/foo" },
    { 5, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/kernel/foo",           -1, "priv5 read /kernel/foo" },
    { 8, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_WRITE, "/kernel/foo",           -1, "priv8 write /kernel/foo" },
    { 9, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/kernel/foo",           -1, "priv9 no-ui read /kernel/foo" },
    { 9, 0, 0, 1, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/kernel/foo",           -1, "priv9+ui plain file op on /kernel (denied; use OB_ReadKernelFile)" },
    { 5, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/kernel",               -1, "priv5 read /kernel (no slash)" },
    { 5, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/kernel/x",      -1, "priv5 read /system/kernel/x" },
    { 5, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/kernel",        -1, "priv5 read /system/kernel (no slash)" },

    /* ============================================================
     * FILE：/system/critical ACL
     * ============================================================ */

    { 0, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/critical/init.obr", -1, "priv0 read /system/critical" },
    { 7, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/critical/init.obr", -1, "priv7 read /system/critical" },
    { 8, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_WRITE, "/system/critical/init.obr", -1, "priv8 write /system/critical" },
    { 5, 1, 0, 0, 5,    OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/critical/init.obr",  0, "pid5 (reserved) read /system/critical" },
    { 8, 1, 0, 0, 5,    OB_RES_FILE, 0, OB_ACCESS_WRITE, "/system/critical/init.obr",  0, "pid5 (reserved) write /system/critical" },
    { 9, 0, 0, 1, 5000, OB_RES_FILE, 0, OB_ACCESS_WRITE, "/system/critical/init.obr",  0, "priv9+ui write /system/critical" },
    { 9, 0, 0, 1, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/critical",           0, "priv9+ui read /system/critical (no slash)" },
    { 8, 1, 1, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/critical/init.obr", -1, "sandbox read /system/critical" },
    { 9, 0, 1, 1, 5000, OB_RES_FILE, 0, OB_ACCESS_WRITE, "/system/critical/init.obr", -1, "sandbox(priv9+ui) write /system/critical" },

    /* ============================================================
     * FILE：权限 1 专用目录
     * ============================================================ */

    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_READ,  "/tmp/priv1_100_abc/",   0, "priv1 read own tmpdir" },
    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_WRITE, "/tmp/priv1_100_abc/f",  0, "priv1 write own tmpdir" },
    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_DELETE,"/tmp/priv1_100_abc/f",  0, "priv1 delete own tmpdir" },
    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_READ,  "/tmp/priv1_101_xyz/",  -1, "priv1 read other's tmpdir" },
    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_READ,  "/tmp/priv1_100",       -1, "priv1 read prefix without underscore" },
    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_READ,  "/home/foo",            -1, "priv1 read /home/foo" },
    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_READ,  "/kernel/foo",          -1, "priv1 read /kernel/foo" },
    { 1, 0, 0, 0, 100, OB_RES_FILE, 0, OB_ACCESS_READ,  "/system/critical/x",   -1, "priv1 read /system/critical/x" },

    /* ============================================================
     * FILE：权限 0（VFS-in-RAM 未实现）
     * ============================================================ */

    { 0, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ, "/any/path",  -2, "priv0 file op (ENOSYS: VFS not implemented)" },
    { 0, 0, 0, 0, 5000, OB_RES_FILE, 0, OB_ACCESS_READ, "/kernel/x", -1, "priv0 /kernel (protected before ENOSYS)" },

    /* ============================================================
     * PROCESS：PID 0-99 保护 + 常规比较
     * ============================================================ */

    { 5, 0, 0, 0, 5000, OB_RES_PROCESS, 5,    OB_ACCESS_READ,  NULL, -1, "priv5 non-reserved caller -> PID 5 (SEC_PID_VIOLATION)" },
    { 8, 0, 0, 0, 5000, OB_RES_PROCESS, 50,   OB_ACCESS_WRITE, NULL, -1, "priv8 caller -> PID 50 (reserved)" },
    { 9, 0, 0, 0, 5000, OB_RES_PROCESS, 1,    OB_ACCESS_WRITE, NULL, -1, "priv9 caller -> PID 1 (reserved, still denied)" },
    { 8, 0, 0, 0, 5,    OB_RES_PROCESS, 50,   OB_ACCESS_READ,  NULL,  0, "pid5 (reserved) -> PID 50 (allowed)" },
    { 5, 0, 0, 0, 5000, OB_RES_PROCESS, 5000, OB_ACCESS_READ,  NULL,  0, "caller -> self (allowed)" },
    { 2, 0, 0, 0, 5000, OB_RES_PROCESS, 6000, OB_ACCESS_READ,  NULL, -1, "priv2 caller -> PID 6000 (insufficient level)" },
    { 6, 0, 0, 0, 5000, OB_RES_PROCESS, 6000, OB_ACCESS_READ,  NULL,  0, "priv6 caller -> PID 6000 (allowed)" },

    /* ============================================================
     * IPC
     * ============================================================ */

    { 0, 0, 0, 0, 5000, OB_RES_IPC, 1, OB_ACCESS_READ,  NULL, -1, "priv0 IPC (denied)" },
    { 1, 0, 0, 0, 5000, OB_RES_IPC, 1, OB_ACCESS_READ,  NULL, -2, "priv1 IPC (ENOSYS: not implemented)" },
    { 5, 0, 0, 0, 5000, OB_RES_IPC, 1, OB_ACCESS_READ,  NULL, -2, "priv5 IPC (ENOSYS: not implemented)" },

    /* ============================================================
     * CONFIG
     * ============================================================ */

    { 3, 0, 0, 0, 5000, OB_RES_CONFIG, 1, OB_ACCESS_WRITE,  NULL, -1, "priv3 write config" },
    { 5, 0, 0, 0, 5000, OB_RES_CONFIG, 1, OB_ACCESS_WRITE,  NULL, -1, "priv5 write config" },
    { 6, 0, 0, 0, 5000, OB_RES_CONFIG, 1, OB_ACCESS_WRITE,  NULL, -1, "priv6 write config (needs UAC; step-9 denies)" },
    { 6, 0, 0, 0, 5000, OB_RES_CONFIG, 1, OB_ACCESS_READ,   NULL,  0, "priv6 read config" },
    { 8, 0, 0, 0, 5000, OB_RES_CONFIG, 1, OB_ACCESS_WRITE,  NULL,  0, "priv8 write config" },
    { 9, 0, 0, 1, 5000, OB_RES_CONFIG, 1, OB_ACCESS_WRITE,  NULL,  0, "priv9+ui write config" },
};

static void make_fake_task(struct task_t *t, const struct perm_test_case *c)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;

    t->pid             = c->caller_pid;
    t->parent_pid      = 0;
    t->privilege_level = c->caller_priv;
    t->isolation_mode  = 0;
    t->sandbox_flags   = c->caller_sandbox;
    t->is_critical     = c->caller_is_critical;
    t->ui_token_valid  = c->caller_ui_token;

    /* mem_domain 清零 —— 选项 A 不依赖其字段 */
    /* security_token 保持一致 */
    t->security_token.level = c->caller_priv;
}

void permission_selftest(void)
{
    int total  = 0;
    int failed = 0;
    int n = (int)(sizeof(g_cases) / sizeof(g_cases[0]));

    serial_printf("[PERM] running %d cases\n", n);

    for (int i = 0; i < n; ++i) {
        const struct perm_test_case *c = &g_cases[i];
        struct task_t fake;
        make_fake_task(&fake, c);

        int rc = check_permission(&fake, c->res_type, c->res_id,
                                  c->access_mode, c->path_hint);

        int ok;
        if (c->expected == 0) {
            ok = (rc == 0);
        } else {
            /* 只要符号为负即视为拒绝，具体值允许 -EPERM / -ENOSYS */
            ok = (rc < 0);
        }

        ++total;
        if (ok) {
            serial_printf("[PERM] OK  : %s (rc=%d)\n", c->desc, rc);
        } else {
            serial_printf("[PERM] FAIL: %s expected=%d got=%d\n",
                          c->desc, c->expected, rc);
            ++failed;
        }
    }

    if (failed == 0) {
        serial_printf("[PERM] selftest OK (%d cases)\n", total);
    } else {
        serial_printf("[PERM] selftest FAILED: %d/%d case(s)\n",
                      failed, total);
    }
}