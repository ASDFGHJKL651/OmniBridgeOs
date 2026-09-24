/*===OmniBridgeOs/kernel/arch/x64/block/page_cache.h===*/
/*
 * 简单页缓存（第 18B 步）。
 *
 * 设计：
 *   - 每个缓存条目 = 一个 4KB 页 = 一个 OBFS 块。
 *   - 键：(block_device*, lba/8)。
 *   - 状态：CLEAN / DIRTY。
 *   - 命中返回内存页指针；未命中从块层加载。
 *
 * 人工必须审查：
 *   - 全局自旋锁保护缓存表；数据访问允许在锁外进行。
 *   - 缓存容量固定，采用简单的 LRU 淘汰。
 *   - 脏页在 sync 时回写。
 */
#ifndef OMNIBRIDGE_BLOCK_PAGE_CACHE_H
#define OMNIBRIDGE_BLOCK_PAGE_CACHE_H

#include "block.h"

#define PCACHE_MAX_ENTRIES  256

struct pcache_entry {
    struct block_device *dev;
    uint64_t             block_no;      /* 4KB 块号 */
    uint8_t             *data;          /* PAGE_SIZE 字节的 DMA 缓冲 */
    uint8_t              dirty;
    uint8_t              valid;
    uint8_t              _pad[2];
    uint64_t             last_used_tick;
    struct pcache_entry *next_lru;
};

void pcache_init(void);

/* 获取（可能加载）一个块。成功返回 0 并 *out_data 指向页内数据。 */
int pcache_get(struct block_device *dev, uint64_t block_no,
               uint8_t **out_data, int for_write);

/* 标记某个已缓存块为脏。 */
int pcache_mark_dirty(struct block_device *dev, uint64_t block_no);

/* 回写单个块（若脏）。 */
int pcache_writeback(struct block_device *dev, uint64_t block_no);

/* 回写所有脏块。 */
int pcache_sync(void);

/* 统计。 */
uint32_t pcache_hit_count(void);
uint32_t pcache_miss_count(void);
uint32_t pcache_dirty_count(void);

/* 失效一个块（例如在 OBFS 元数据直接写入后）。 */
void pcache_invalidate(struct block_device *dev, uint64_t block_no);

#endif /* OMNIBRIDGE_BLOCK_PAGE_CACHE_H */
/*===OmniBridgeOs/kernel/arch/x64/block/page_cache.h 结束===*/