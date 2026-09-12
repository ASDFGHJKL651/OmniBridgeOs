#include "pmm.h"
#include "printk.h"
#include "serial.h"
#include "spinlock.h"

/*
 * 物理内存伙伴系统 —— SMP 安全版本。
 *
 * SMP 安全设计（人工必须审查）：
 *   - 全部伙伴系统操作（分配/释放/统计）均持有单一全局自旋锁 pmm_lock。
 *     这是最简单也最保守的方案；在单核阶段开销可忽略，双核阶段会有一
 *     定争用，但正确性优先。
 *   - mark_region_usable 只在 pmm_init 期间调用，此时尚无并发，无需锁。
 *   - free_count 的读取也加锁，保证 64 位读写在 32 位原子性不会出现撕裂
 *     （x86_64 上 64 位自然对齐读写在硬件层已原子，但锁仍保证与写者的
 *     一致性，避免编译器重排）。
 *   - 后续可以加入每 CPU 页帧缓存以减少争用；本步先给出正确版本。
 *
 * 内存序：所有对 mem_map 与 free_lists 的访问都在 pmm_lock 内进行，不
 * 需要额外的 barrier。
 *
 * free_count 记账不变量（人工必须审查）：
 *   - free_count 恒等于「当前空闲物理页总数」。
 *   - 分配一个 order-k 块：free_count 减 2^k（无论是否发生拆分）。
 *   - 释放一个 order-k 块：free_count 增 2^k（无论是否发生合并）。
 *   - 因此 pmm_free_pages 的增量必须始终基于「释放时的原始 order」，
 *     不能在合并循环里用被抬高的 order —— 否则每次合并都会重复计算
 *     buddy 已计入的页数，导致 free_count 单调膨胀（超过 total_pages）。
 */

struct page mem_map[MAX_PAGES];

static struct page *free_lists[MAX_ORDER];
static uint64_t total_pages = 0;
static uint64_t free_count  = 0;

/* 全局伙伴系统锁 */
static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline uint64_t pfn_of(const struct page *p)
{
    return (uint64_t)(p - mem_map);
}

static inline struct page *buddy_of(const struct page *p, int order)
{
    uint64_t pfn = pfn_of(p) ^ (1ULL << order);
    return &mem_map[pfn];
}

/* 调用者必须持有 pmm_lock */
static void free_list_push(int order, struct page *p)
{
    p->order = (uint8_t)order;
    p->flags = PG_FREE;
    p->next  = free_lists[order];
    free_lists[order] = p;
}

/* 调用者必须持有 pmm_lock */
static struct page *free_list_pop(int order)
{
    struct page *p = free_lists[order];
    if (!p) return 0;
    free_lists[order] = p->next;
    p->next  = 0;
    p->flags = 0;
    return p;
}

/* 仅 pmm_init 调用，无并发 */
static void mark_region_usable(uint64_t start_phys, uint64_t pages)
{
    uint64_t start_pfn = start_phys >> PAGE_SHIFT;
    uint64_t end_pfn   = start_pfn + pages;
    if (end_pfn > MAX_PAGES) end_pfn = MAX_PAGES;
    if (start_pfn >= end_pfn) return;

    uint64_t pfn = start_pfn;
    while (pfn < end_pfn) {
        int order = MAX_ORDER - 1;
        while (order > 0) {
            uint64_t block_pages = 1ULL << order;
            if ((pfn & (block_pages - 1)) == 0 && pfn + block_pages <= end_pfn)
                break;
            --order;
        }
        uint64_t block_pages = 1ULL << order;
        struct page *p = &mem_map[pfn];

        for (uint64_t i = 0; i < block_pages; ++i) {
            mem_map[pfn + i].order      = 0;
            mem_map[pfn + i].flags      = 0;
            mem_map[pfn + i].refcount   = 0;
            mem_map[pfn + i].slab_cache = 0;
            mem_map[pfn + i].slab_free  = 0;
            mem_map[pfn + i].next       = 0;
        }
        free_list_push(order, p);
        free_count  += block_pages;
        total_pages += block_pages;
        pfn += block_pages;
    }
}

void pmm_init(const struct ob_boot_info *bi)
{
    /* 初始化 mem_map（无并发） */
    for (uint64_t i = 0; i < MAX_PAGES; ++i) {
        mem_map[i].order      = 0;
        mem_map[i].flags      = PG_RESERVED;
        mem_map[i].refcount   = 1;
        mem_map[i].slab_cache = 0;
        mem_map[i].slab_free  = 0;
        mem_map[i].next       = 0;
    }
    for (int i = 0; i < MAX_ORDER; ++i) free_lists[i] = 0;

    spin_lock_init(&pmm_lock);

    total_pages = 0;
    free_count  = 0;

    if (!bi) {
        printk("[PMM] no boot_info, no pages registered\n");
        return;
    }

    for (uint32_t i = 0; i < bi->entry_count; ++i) {
        const struct ob_memory_entry *e = &bi->memory_map[i];
        uint64_t phys  = e->physical_start;
        uint64_t pages = e->number_of_pages;

        int usable = (e->type == 7 /* EfiConventionalMemory */) ||
                     (e->type == 3 /* EfiBootServicesCode */)     ||
                     (e->type == 4 /* EfiBootServicesData */);
        if (!usable) continue;

        /* 保留低 1MB（含 BIOS/VGA 等） */
        if (phys < 0x100000) {
            uint64_t skip = 0x100000 - phys;
            uint64_t skip_pages = (skip + PAGE_SIZE - 1) / PAGE_SIZE;
            if (skip_pages >= pages) continue;
            phys  += skip_pages * PAGE_SIZE;
            pages -= skip_pages;
        }

        mark_region_usable(phys, pages);
    }

    printk("[PMM] usable pages: %llu, free: %llu\n",
           (unsigned long long)total_pages,
           (unsigned long long)free_count);
}

struct page *pmm_alloc_pages(int order)
{
    if (order < 0 || order >= MAX_ORDER) return 0;

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    int o = order;
    while (o < MAX_ORDER && !free_lists[o]) ++o;
    if (o == MAX_ORDER) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return 0;
    }

    struct page *p = free_list_pop(o);
    while (o > order) {
        --o;
        struct page *buddy = buddy_of(p, o);
        free_list_push(o, buddy);
    }
    p->order      = (uint8_t)order;
    p->flags      = 0;
    p->refcount   = 1;
    p->slab_cache = 0;
    p->slab_free  = 0;

    /*
     * 记账：无论是否拆分，从空闲池里取走的用户可见页数恒为 2^order。
     * 拆分只是把一个大块重新组织成两个小块，不改变空闲页总数。
     */
    free_count -= (1ULL << order);

    spin_unlock_irqrestore(&pmm_lock, flags);
    return p;
}

void pmm_free_pages(struct page *p, int order)
{
    if (!p || order < 0 || order >= MAX_ORDER) return;

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    if (p->flags & PG_FREE) {
        /* 双重释放 */
        spin_unlock_irqrestore(&pmm_lock, flags);
        printk("[PMM] double free detected, ignoring\n");
        return;
    }

    /*
     * 关键：先保存原始 order。
     *
     * 合并循环会逐步抬高 order，但 free_count 的增量必须始终等于
     * 「本次释放的原始页数」= 1 << original_order。原因：
     *   - buddy 若已空闲，已经计入 free_count；合并只是把两者重组成
     *     一个更大的块，不改变空闲页总数。
     *   - 若使用被抬高后的 order，每次合并都会额外多计 buddy 的页数，
     *     导致 free_count 单调膨胀，压力测试下会超过 total_pages。
     */
    const int original_order = order;

    while (order < MAX_ORDER - 1) {
        struct page *buddy = buddy_of(p, order);
        if (!(buddy->flags & PG_FREE) || buddy->order != order) break;

        /* 从 free_lists[order] 摘下 buddy（O(n) 简单实现） */
        struct page **pp = &free_lists[order];
        while (*pp && *pp != buddy) pp = &(*pp)->next;
        if (*pp == buddy) {
            *pp = buddy->next;
            buddy->next  = 0;
        } else {
            break; /* 异常：不在链表里，不合并 */
        }
        if (buddy < p) p = buddy;
        ++order;
    }
    free_list_push(order, p);
    free_count += (1ULL << original_order);

    /* 不变量检测：free_count 绝不应超过 total_pages。触发即计数器损坏。 */
    if (free_count > total_pages) {
        printk(KERN_ERR
               "[PMM] free_count %llu > total_pages %llu "
               "(orig_order=%d, merged_order=%d) — counter corruption\n",
               (unsigned long long)free_count,
               (unsigned long long)total_pages,
               original_order, order);
        /* 保守截断，避免下游读到大得离谱的数字；同时保留错误信息便于定位 */
        free_count = total_pages;
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_total_pages(void)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t v = total_pages;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return v;
}

uint64_t pmm_free_pages_count(void)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t v = free_count;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return v;
}