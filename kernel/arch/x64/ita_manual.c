/*===OmniBridgeOs/kernel/arch/x64/ita_manual.c===*/
#include "ita_manual.h"
#include "audit.h"
#include "sha384.h"
#include "spinlock.h"
#include "serial.h"
#include "permission.h"

static struct ita_manual_entry g_entries[ITA_MANUAL_MAX_ENTRIES];
static uint32_t g_count = 0;
static spinlock_t g_lock = SPINLOCK_INIT;
static int g_inited = 0;

/* ---------- 本地字符串工具（避免依赖 libc） ---------- */

static size_t str_len_(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static int str_eq_(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static uint64_t rdtsc_(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* ---------- 初始化 ---------- */

void ita_manual_init(void)
{
    if (g_inited) return;

    spin_lock_init(&g_lock);

    for (uint32_t i = 0; i < ITA_MANUAL_MAX_ENTRIES; ++i) {
        uint8_t *p = (uint8_t *)&g_entries[i];
        for (uint32_t k = 0; k < sizeof(g_entries[i]); ++k) p[k] = 0;
    }
    g_count = 0;
    g_inited = 1;

    serial_printf("[ITA-MANUAL] init: max_entries=%u path_max=%u\n",
                  (unsigned)ITA_MANUAL_MAX_ENTRIES,
                  (unsigned)ITA_MANUAL_PATH_MAX);
}

/* ---------- 内部：查找 ----------
 * 调用者必须持有 g_lock。
 * 命中返回 1 并填充 *out_idx；未命中返回 0。 */
static int find_locked_(const char *path, uint32_t *out_idx)
{
    for (uint32_t i = 0; i < g_count; ++i) {
        if (str_eq_(g_entries[i].path, path)) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

/* ---------- 添加 ---------- */

int ita_manual_add(struct task_t *caller,
                   const char *path,
                   const uint8_t sha384[48])
{
    if (!caller || !path || !sha384) return OB_EINVAL;
    if (path[0] == '\0') return OB_EINVAL;

    size_t plen = str_len_(path);
    if (plen == 0 || plen >= ITA_MANUAL_PATH_MAX) return OB_EINVAL;

    /* 权限检查（人工必须审查）：
     *   - 必须权限 9（内核管理器）
     *   - 必须持有有效 UI 令牌
     *   - 沙盒进程一律拒绝（最高优先级） */
    if (caller->sandbox_flags != 0) return OB_EPERM;
    if (caller->privilege_level != 9) return OB_EPERM;
    if (!caller->ui_token_valid)     return OB_EPERM;

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);

    uint32_t idx = 0;
    int found = find_locked_(path, &idx);

    if (!found) {
        if (g_count >= ITA_MANUAL_MAX_ENTRIES) {
            spin_unlock_irqrestore(&g_lock, irqf);
            return OB_ENOMEM;
        }
        idx = g_count++;
    }

    struct ita_manual_entry *e = &g_entries[idx];

    /* 拷贝路径（含结尾 NUL） */
    for (size_t i = 0; i < plen; ++i) e->path[i] = path[i];
    e->path[plen] = '\0';

    /* 拷贝哈希 */
    for (int i = 0; i < 48; ++i) e->sha384[i] = sha384[i];

    e->added_by_pid = caller->pid;
    e->tick         = rdtsc_();

    spin_unlock_irqrestore(&g_lock, irqf);

    /* CRITICAL 审计（人工必须审查）：
     *   - 复用 AUDIT_EV_CRITICAL_ACCESS，mode = OB_ACCESS_WRITE。
     *   - arg0 = 哈希前 8 字节（大端组合），供事后追踪。 */
    uint64_t hash_prefix = 0;
    for (int i = 0; i < 8; ++i) {
        hash_prefix = (hash_prefix << 8) | (uint64_t)sha384[i];
    }
    audit_event(AUDIT_EV_CRITICAL_ACCESS, AUDIT_LVL_CRITICAL,
                caller->pid, hash_prefix, 0,
                OB_ACCESS_WRITE, path);

    serial_printf("[ITA-MANUAL] add path=%s by pid=%llu\n",
                  path, (unsigned long long)caller->pid);
    return 0;
}

/* ---------- 验证 ---------- */

int ita_manual_verify(const char *path, const void *buf, uint64_t size)
{
    if (!path) return OB_EINVAL;

    uint8_t expected[48];

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);

    uint32_t idx = 0;
    if (!find_locked_(path, &idx)) {
        spin_unlock_irqrestore(&g_lock, irqf);
        return OB_ENOENT;
    }
    for (int i = 0; i < 48; ++i) expected[i] = g_entries[idx].sha384[i];

    spin_unlock_irqrestore(&g_lock, irqf);

    /* 哈希计算在锁外完成，避免长时间持锁 */
    uint8_t digest[48];
    sha384(buf, size, digest);

    uint8_t diff = 0;
    for (int i = 0; i < 48; ++i) diff |= (uint8_t)(digest[i] ^ expected[i]);
    return (diff == 0) ? 0 : OB_EACCES;
}

/* ---------- 查询 / 清空 / 计数 / 遍历 ---------- */

int ita_manual_contains(const char *path)
{
    if (!path) return 0;

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);
    uint32_t idx = 0;
    int found = find_locked_(path, &idx);
    spin_unlock_irqrestore(&g_lock, irqf);
    return found;
}

void ita_manual_clear(void)
{
    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);
    for (uint32_t i = 0; i < ITA_MANUAL_MAX_ENTRIES; ++i) {
        uint8_t *p = (uint8_t *)&g_entries[i];
        for (uint32_t k = 0; k < sizeof(g_entries[i]); ++k) p[k] = 0;
    }
    g_count = 0;
    spin_unlock_irqrestore(&g_lock, irqf);
}

uint32_t ita_manual_count(void)
{
    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);
    uint32_t n = g_count;
    spin_unlock_irqrestore(&g_lock, irqf);
    return n;
}

void ita_manual_iterate(ita_manual_iter_cb cb, void *arg)
{
    if (!cb) return;
    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);
    for (uint32_t i = 0; i < g_count; ++i) {
        cb(&g_entries[i], arg);
    }
    spin_unlock_irqrestore(&g_lock, irqf);
}
/*===OmniBridgeOs/kernel/arch/x64/ita_manual.c 结束===*/