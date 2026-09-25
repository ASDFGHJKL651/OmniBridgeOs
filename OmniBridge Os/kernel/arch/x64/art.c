/*===OmniBridgeOs/kernel/arch/x64/art.c===*/
#include "art.h"
#include "sha256.h"
#include "pmm.h"
#include "vmm.h"
#include "spinlock.h"
#include "serial.h"
#include "task.h"
#include "permission.h"

static struct art_entry *g_entries  = 0;
static struct page      *g_table_pg = 0;
static int               g_table_order = -1;
static uint32_t          g_count = 0;
static uint64_t          g_generation = 0;
static spinlock_t        g_lock = SPINLOCK_INIT;
static int               g_inited = 0;

/* ---------- 本地工具 ---------- */

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

/* ---------- 哈希 ----------
 * SHA-256 前 8 字节，big-endian 组合。
 *
 * 人工必须审查：与 §12.2 一致（64 位截断）。 */
uint64_t art_hash_name(const char *api_name)
{
    if (!api_name) return 0;

    uint8_t h[32];
    sha256(api_name, (uint64_t)str_len_(api_name), h);

    uint64_t k = 0;
    for (int i = 0; i < 8; ++i) {
        k = (k << 8) | (uint64_t)h[i];
    }
    return k;
}

/* ---------- 初始化 ---------- */

void art_init(void)
{
    if (g_inited) return;

    spin_lock_init(&g_lock);

    const size_t bytes = (size_t)ART_MAX_ENTRIES * sizeof(struct art_entry);
    const uint64_t pages = ((uint64_t)bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    int order = 0;
    while (order < MAX_ORDER && ((uint64_t)1 << order) < pages) {
        order++;
    }
    if (order >= MAX_ORDER) {
        serial_printf("[ART] FATAL: table too large (%llu bytes, need %llu pages)\n",
                      (unsigned long long)bytes,
                      (unsigned long long)pages);
        return;
    }

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) {
        serial_printf("[ART] FATAL: pmm_alloc_pages(order=%d) failed "
                      "(%llu bytes)\n",
                      order, (unsigned long long)bytes);
        return;
    }

    uint64_t pa = page_to_phys(pg);
    g_entries = (struct art_entry *)(uintptr_t)(DIRECTMAP_BASE + pa);

    uint8_t *p = (uint8_t *)g_entries;
    uint64_t alloc_bytes = PAGE_SIZE << order;
    for (uint64_t i = 0; i < alloc_bytes; ++i) p[i] = 0;

    g_table_pg    = pg;
    g_table_order = order;
    g_count       = 0;
    g_generation  = 1;
    g_inited      = 1;

    serial_printf("[ART] init: entries_cap=%u entry_size=%u "
                  "table_bytes=%llu order=%d alloc_bytes=%llu hash=SHA256\n",
                  (unsigned)ART_MAX_ENTRIES,
                  (unsigned)sizeof(struct art_entry),
                  (unsigned long long)bytes,
                  order,
                  (unsigned long long)alloc_bytes);
}

uint64_t art_base_addr(void)
{
    if (!g_inited || !g_entries) return 0;
    return (uint64_t)(uintptr_t)g_entries;
}

/* ---------- 内部：查找 ---------- */
static uint32_t find_locked_(uint64_t key, const char *name)
{
    for (uint32_t i = 0; i < g_count; ++i) {
        if (g_entries[i].key == key &&
            str_eq_(g_entries[i].name, name)) {
            return i;
        }
    }
    return (uint32_t)-1;
}

/* ---------- 注册 ---------- */

int art_register(const char *api_name, void *func_ptr, uint32_t flags)
{
    if (!api_name || api_name[0] == '\0') return OB_EINVAL;
    size_t nlen = str_len_(api_name);
    if (nlen >= ART_NAME_MAX) return OB_EINVAL;
    if (!g_inited) return OB_ENOMEM;

    uint64_t key = art_hash_name(api_name);

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);

    uint32_t idx = find_locked_(key, api_name);

    if (idx == (uint32_t)-1) {
        if (g_count >= ART_MAX_ENTRIES) {
            spin_unlock_irqrestore(&g_lock, irqf);
            return OB_ENOMEM;
        }
        idx = g_count++;
        g_entries[idx].key = key;
        for (size_t i = 0; i < nlen; ++i)
            g_entries[idx].name[i] = api_name[i];
        g_entries[idx].name[nlen] = '\0';
    }

    g_entries[idx].func_ptr = func_ptr;
    g_entries[idx].flags    = flags;
    g_generation++;

    spin_unlock_irqrestore(&g_lock, irqf);
    return 0;
}

/* ---------- 查询 ---------- */

int art_lookup(const char *api_name, struct art_entry *out)
{
    if (!api_name || !out) return OB_EINVAL;
    if (!g_inited) return OB_ENOENT;

    uint64_t key = art_hash_name(api_name);

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);

    uint32_t idx = find_locked_(key, api_name);
    int rc = 0;
    if (idx == (uint32_t)-1) {
        rc = OB_ENOENT;
    } else {
        const uint8_t *src = (const uint8_t *)&g_entries[idx];
        uint8_t *dst = (uint8_t *)out;
        for (uint32_t i = 0; i < (uint32_t)sizeof(*out); ++i) {
            dst[i] = src[i];
        }
    }

    spin_unlock_irqrestore(&g_lock, irqf);
    return rc;
}

/* ---------- 遍历 ---------- */
void art_walk(void (*cb)(const struct art_entry *e, void *arg), void *arg)
{
    if (!cb || !g_inited) return;

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);
    for (uint32_t i = 0; i < g_count; ++i) {
        cb(&g_entries[i], arg);
    }
    spin_unlock_irqrestore(&g_lock, irqf);
}

uint32_t art_count(void)
{
    if (!g_inited) return 0;

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);
    uint32_t n = g_count;
    spin_unlock_irqrestore(&g_lock, irqf);
    return n;
}

void art_stats(struct art_table_stats *out)
{
    if (!out) return;
    if (!g_inited) {
        out->count = 0;
        out->generation = 0;
        return;
    }

    uint64_t irqf;
    spin_lock_irqsave(&g_lock, &irqf);
    out->count      = g_count;
    out->generation = g_generation;
    spin_unlock_irqrestore(&g_lock, irqf);
}

/* ---------- 动态扩展 ---------- */

int ob_register_api_alias(struct task_t *caller,
                          const char *api_name,
                          void *func_ptr,
                          uint32_t flags,
                          const char *target_path)
{
    if (!caller) return OB_EPERM;
    if (caller->sandbox_flags != 0) return OB_EPERM;
    if (caller->privilege_level < 6) return OB_EPERM;

    if (target_path && target_path[0] != '\0') {
        int p_kernel = path_is_kernel_protected(target_path);
        int p_crit   = path_is_critical_dir(target_path);

        if (p_kernel) return OB_EPERM;
        if (p_crit) {
            if (!(caller->privilege_level == 9 && caller->ui_token_valid)) {
                return OB_EPERM;
            }
        }
    }

    int rc = art_register(api_name, func_ptr, flags);
    if (rc == 0) {
        serial_printf("[ART] ob_register_api_alias: name=%s by pid=%llu\n",
                      api_name ? api_name : "(null)",
                      (unsigned long long)caller->pid);
    }
    return rc;
}