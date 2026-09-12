#ifndef OMNIBRIDGE_PMM_H
#define OMNIBRIDGE_PMM_H

#include <stdint.h>
#include "boot.h"
#include "spinlock.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1ULL << PAGE_SHIFT)
#define PAGE_MASK  (~(PAGE_SIZE - 1))

#define MAX_ORDER  10
#define MAX_PAGES  131072   /* 512 MB / 4 KB */

/*
 * 物理页元数据（struct page）。
 *
 * 字段说明（人工必须审查）：
 *   - order：伙伴系统中的阶；当 PG_SLAB 时也用于记录 slab 占用的页数-1
 *   - flags：PG_FREE / PG_RESERVED / PG_SLAB
 *   - refcount：引用计数（伙伴系统为 1；SLAB 中记录已分配对象数）
 *   - slab_cache：当 PG_SLAB 时指向该 slab 所属的 struct kmem_cache
 *   - slab_free：当 PG_SLAB 时指向该 slab 的空闲对象链表头
 *   - next：用于伙伴系统 free_lists 或 kmem_cache 的 partial 链表
 *
 * 大小：1+1+2+4+8+8+8 = 32 字节。mem_map[131072] 约 4 MB。
 */
struct page {
    uint8_t  order;
    uint8_t  flags;
    uint16_t _pad;
    uint32_t refcount;
    void    *slab_cache;
    void    *slab_free;
    struct page *next;
};

#define PG_FREE     0x01
#define PG_RESERVED 0x02
#define PG_SLAB     0x04

void pmm_init(const struct ob_boot_info *bi);

/*
 * 伙伴系统分配/释放（SMP 安全，内部加自旋锁）。
 *   - 返回页头 page*，失败返回 NULL
 *   - 释放时 order 必须与分配时一致
 */
struct page *pmm_alloc_pages(int order);
void         pmm_free_pages(struct page *p, int order);

/* 统计信息 */
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);

/* phys <-> page 转换（依赖恒等映射覆盖内核可见物理内存） */
static inline uint64_t page_to_phys(const struct page *p)
{
    extern struct page mem_map[];
    return (uint64_t)(p - mem_map) << PAGE_SHIFT;
}

static inline struct page *phys_to_page(uint64_t phys)
{
    extern struct page mem_map[];
    return &mem_map[phys >> PAGE_SHIFT];
}

#endif /* OMNIBRIDGE_PMM_H */