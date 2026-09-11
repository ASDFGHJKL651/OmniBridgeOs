#ifndef OMNIBRIDGE_PMM_H
#define OMNIBRIDGE_PMM_H

#include <stdint.h>
#include "boot.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1ULL << PAGE_SHIFT)
#define PAGE_MASK  (~(PAGE_SIZE - 1))

#define MAX_ORDER  10
#define MAX_PAGES  131072   /* 512 MB / 4 KB */

struct page {
    uint8_t  order;
    uint8_t  flags;
    uint16_t slab_class;
    uint32_t refcount;
    struct page *next;
};

#define PG_FREE     0x01
#define PG_RESERVED 0x02
#define PG_SLAB     0x04

void pmm_init(const struct ob_boot_info *bi);

/* 返回页头 page*，失败返回 NULL */
struct page *pmm_alloc_pages(int order);
void pmm_free_pages(struct page *p, int order);

uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);

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

#endif