/*===OmniBridgeOs/kernel/arch/x64/compat_handle.c===*/
/*
 * 兼容层句柄表实现。
 *
 * ★ 第 18 步修复（人工必须审查）：
 *   本结构体 sizeof ≈ 8208 字节，远超 kmalloc() 单次上限（PAGE_SIZE = 4096）。
 *   kmalloc 在 size > PAGE_SIZE 时直接返回 NULL，导致 create 失败、测试
 *   报 "FAIL: create table"。
 *
 *   修复方式与 art.c 一致：使用 pmm_alloc_pages(order) 分配连续物理页，
 *   通过 DirectMap 虚拟地址访问；销毁时通过 phys_to_page 反推物理页头，
 *   按同一 order 归还伙伴系统。
 *
 *   order 由 sizeof 在编译期决定，分配与释放路径调用同一个 htab_order()，
 *   保证两者一致。
 */
#include "compat_handle.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"

/* ---------- 分配尺寸计算 ---------- */

/* 根据 sizeof(struct compat_handle_table) 计算需要的伙伴系统 order。
 * 返回 -1 表示需求超过 MAX_ORDER 覆盖范围（正常情况下不会发生）。 */
static int htab_order(void)
{
    const size_t bytes = sizeof(struct compat_handle_table);
    const size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    int order = 0;
    while (order < MAX_ORDER && ((size_t)1 << order) < pages) {
        order++;
    }
    if (order >= MAX_ORDER) return -1;
    return order;
}

/* 用 0 填充整个分配区域（不限于 sizeof，避免残留数据） */
static void zero_region(void *p, uint64_t bytes)
{
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < bytes; ++i) b[i] = 0;
}

/* ---------- 公共接口 ---------- */

struct compat_handle_table *compat_handle_table_create(void)
{
    int order = htab_order();
    if (order < 0) {
        serial_printf("[COMPAT-HANDLE] FATAL: table too large "
                      "(sizeof=%llu bytes)\n",
                      (unsigned long long)sizeof(struct compat_handle_table));
        return 0;
    }

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) {
        serial_printf("[COMPAT-HANDLE] pmm_alloc_pages(order=%d) failed "
                      "(need %llu bytes)\n",
                      order,
                      (unsigned long long)sizeof(struct compat_handle_table));
        return 0;
    }

    uint64_t pa = page_to_phys(pg);
    struct compat_handle_table *ht =
        (struct compat_handle_table *)(uintptr_t)(DIRECTMAP_BASE + pa);

    /* 清零整块分配区域（不只 sizeof 字节） */
    uint64_t alloc_bytes = PAGE_SIZE << order;
    zero_region(ht, alloc_bytes);

    spin_lock_init(&ht->lock);
    ht->next_handle = COMPAT_HANDLE_BASE;
    ht->count       = 0;

    /* 预置 stdin / stdout / stderr，模拟 fd 0/1/2 */
    ht->entries[0].handle        = 0;
    ht->entries[0].kernel_handle = 0;
    ht->entries[0].type          = OB_RES_FILE;
    ht->entries[0].flags         = 0;
    ht->entries[0].in_use        = 1;

    ht->entries[1].handle        = 1;
    ht->entries[1].kernel_handle = 1;
    ht->entries[1].type          = OB_RES_FILE;
    ht->entries[1].flags         = 0;
    ht->entries[1].in_use        = 1;

    ht->entries[2].handle        = 2;
    ht->entries[2].kernel_handle = 2;
    ht->entries[2].type          = OB_RES_FILE;
    ht->entries[2].flags         = 0;
    ht->entries[2].in_use        = 1;

    ht->count = 3;
    return ht;
}

void compat_handle_table_destroy(struct compat_handle_table *ht)
{
    if (!ht) return;

    /* 反推物理地址：ht 的虚拟地址必然落在 DirectMap 区间 */
    uint64_t pa = (uint64_t)(uintptr_t)ht - DIRECTMAP_BASE;
    struct page *pg = phys_to_page(pa);
    int order = htab_order();

    if (order < 0) {
        /* 理论上不可能：create 时已校验。防御性返回，避免误归还错阶。 */
        serial_printf("[COMPAT-HANDLE] WARN: destroy with bad order, "
                      "table leaked\n");
        return;
    }

    /* 先清零表头锁与计数，避免归还后残留指针被误用 */
    ht->next_handle = 0;
    ht->count       = 0;
    spin_lock_init(&ht->lock);

    pmm_free_pages(pg, order);
}

uint32_t compat_handle_alloc(struct compat_handle_table *ht,
                             uint64_t kernel_handle, uint32_t type,
                             uint32_t flags)
{
    if (!ht) return COMPAT_INVALID_HANDLE;

    uint64_t irqf;
    spin_lock_irqsave(&ht->lock, &irqf);

    /* 从 next_handle 起环绕扫描空闲槽，最多尝试 COMPAT_HANDLE_MAX 次 */
    for (uint32_t attempt = 0; attempt < COMPAT_HANDLE_MAX; ++attempt) {
        uint32_t idx = ht->next_handle % COMPAT_HANDLE_MAX;
        ht->next_handle++;

        if (!ht->entries[idx].in_use) {
            uint32_t handle;
            /* 槽 0/1/2 对应预置 fd 值；创建时已占用，不会命中此处。
             * 其余槽的句柄值 = COMPAT_HANDLE_BASE + idx。 */
            if (idx == 0)      handle = 0;
            else if (idx == 1) handle = 1;
            else if (idx == 2) handle = 2;
            else               handle = COMPAT_HANDLE_BASE + idx;

            ht->entries[idx].handle        = handle;
            ht->entries[idx].kernel_handle = kernel_handle;
            ht->entries[idx].type          = type;
            ht->entries[idx].flags         = flags;
            ht->entries[idx].in_use        = 1;
            ht->count++;

            spin_unlock_irqrestore(&ht->lock, irqf);
            return handle;
        }
    }

    spin_unlock_irqrestore(&ht->lock, irqf);
    return COMPAT_INVALID_HANDLE;
}

int compat_handle_free(struct compat_handle_table *ht, uint32_t handle)
{
    if (!ht) return -1;

    uint64_t irqf;
    spin_lock_irqsave(&ht->lock, &irqf);

    int found = 0;
    for (uint32_t i = 0; i < COMPAT_HANDLE_MAX; ++i) {
        if (ht->entries[i].in_use && ht->entries[i].handle == handle) {
            ht->entries[i].in_use        = 0;
            ht->entries[i].kernel_handle = 0;
            ht->entries[i].flags         = 0;
            if (ht->count > 0) ht->count--;
            found = 1;
            break;
        }
    }

    spin_unlock_irqrestore(&ht->lock, irqf);
    return found ? 0 : -1;
}

int compat_handle_lookup(struct compat_handle_table *ht, uint32_t handle,
                         struct compat_handle_entry *out)
{
    if (!ht || !out) return -1;

    uint64_t irqf;
    spin_lock_irqsave(&ht->lock, &irqf);

    int found = 0;
    for (uint32_t i = 0; i < COMPAT_HANDLE_MAX; ++i) {
        if (ht->entries[i].in_use && ht->entries[i].handle == handle) {
            *out = ht->entries[i];
            found = 1;
            break;
        }
    }

    spin_unlock_irqrestore(&ht->lock, irqf);
    return found ? 0 : -1;
}

uint32_t compat_handle_count(struct compat_handle_table *ht)
{
    if (!ht) return 0;

    uint64_t irqf;
    spin_lock_irqsave(&ht->lock, &irqf);
    uint32_t n = ht->count;
    spin_unlock_irqrestore(&ht->lock, irqf);
    return n;
}
/*===OmniBridgeOs/kernel/arch/x64/compat_handle.c 结束===*/