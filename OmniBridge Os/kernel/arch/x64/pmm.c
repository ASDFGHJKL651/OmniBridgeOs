#include "pmm.h"
#include "printk.h"
#include "serial.h"

struct page mem_map[MAX_PAGES];

static struct page *free_lists[MAX_ORDER];
static uint64_t total_pages = 0;
static uint64_t free_count  = 0;

static inline uint64_t pfn_of(const struct page *p)
{
    return (uint64_t)(p - mem_map);
}

static inline struct page *buddy_of(const struct page *p, int order)
{
    uint64_t pfn = pfn_of(p) ^ (1ULL << order);
    return &mem_map[pfn];
}

static void free_list_push(int order, struct page *p)
{
    p->order = (uint8_t)order;
    p->flags = PG_FREE;
    p->next  = free_lists[order];
    free_lists[order] = p;
}

static struct page *free_list_pop(int order)
{
    struct page *p = free_lists[order];
    if (!p) return 0;
    free_lists[order] = p->next;
    p->next  = 0;
    p->flags = 0;
    return p;
}

/* 扫描内存图：把可用区域合并成尽可能大的块 */
static void mark_region_usable(uint64_t start_phys, uint64_t pages)
{
    uint64_t start_pfn = start_phys >> PAGE_SHIFT;
    uint64_t end_pfn   = start_pfn + pages;
    if (end_pfn > MAX_PAGES) end_pfn = MAX_PAGES;
    if (start_pfn >= end_pfn) return;

    uint64_t pfn = start_pfn;
    while (pfn < end_pfn) {
        /* 从最大对齐的阶开始尝试 */
        int order = MAX_ORDER - 1;
        while (order > 0) {
            uint64_t block_pages = 1ULL << order;
            if ((pfn & (block_pages - 1)) == 0 && pfn + block_pages <= end_pfn)
                break;
            --order;
        }
        uint64_t block_pages = 1ULL << order;
        struct page *p = &mem_map[pfn];
        /* 清理可能残留的旧状态 */
        for (uint64_t i = 0; i < block_pages; ++i) {
            mem_map[pfn + i].order = 0;
            mem_map[pfn + i].flags = 0;
            mem_map[pfn + i].slab_class = 0;
            mem_map[pfn + i].refcount = 0;
            mem_map[pfn + i].next = 0;
        }
        free_list_push(order, p);
        free_count  += block_pages;
        total_pages += block_pages;
        pfn += block_pages;
    }
}

void pmm_init(const struct ob_boot_info *bi)
{
    for (uint64_t i = 0; i < MAX_PAGES; ++i) {
        mem_map[i].order = 0;
        mem_map[i].flags = PG_RESERVED;
        mem_map[i].slab_class = 0;
        mem_map[i].refcount = 1;
        mem_map[i].next = 0;
    }
    for (int i = 0; i < MAX_ORDER; ++i) free_lists[i] = 0;

    total_pages = 0;
    free_count  = 0;

    for (uint32_t i = 0; i < bi->entry_count; ++i) {
        const struct ob_memory_entry *e = &bi->memory_map[i];
        uint64_t phys = e->physical_start;
        uint64_t pages = e->number_of_pages;

        /* 只用 Conventional + BootServices（ExitBootServices 后归内核） */
        int usable = (e->type == 7 /* EfiConventionalMemory */) ||
                     (e->type == 3 /* EfiBootServicesCode */) ||
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

    int o = order;
    while (o < MAX_ORDER && !free_lists[o]) ++o;
    if (o == MAX_ORDER) return 0;

    struct page *p = free_list_pop(o);
    while (o > order) {
        --o;
        struct page *buddy = buddy_of(p, o);
        free_list_push(o, buddy);
    }
    p->order = (uint8_t)order;
    p->flags = 0;
    p->refcount = 1;
    free_count -= (1ULL << order);
    return p;
}

void pmm_free_pages(struct page *p, int order)
{
    if (!p || order < 0 || order >= MAX_ORDER) return;
    if (p->flags & PG_FREE) {
        /* 双重释放 */
        printk("[PMM] double free detected, ignoring\n");
        return;
    }

    while (order < MAX_ORDER - 1) {
        struct page *buddy = buddy_of(p, order);
        if (!(buddy->flags & PG_FREE) || buddy->order != order) break;

        /* 从 free_lists[order] 摘下 buddy（O(n) 简单实现） */
        struct page **pp = &free_lists[order];
        while (*pp && *pp != buddy) pp = &(*pp)->next;
        if (*pp == buddy) {
            *pp = buddy->next;
            buddy->next = 0;
        } else {
            break; /* 异常：不在链表里，不合并 */
        }
        if (buddy < p) p = buddy;
        ++order;
    }
    free_list_push(order, p);
    free_count += (1ULL << order);
}

uint64_t pmm_total_pages(void)      { return total_pages; }
uint64_t pmm_free_pages_count(void) { return free_count; }