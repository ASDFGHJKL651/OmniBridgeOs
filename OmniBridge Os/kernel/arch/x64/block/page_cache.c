/*===OmniBridgeOs/kernel/arch/x64/block/page_cache.c===*/
#include "page_cache.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "spinlock.h"

struct pcache_slot {
    struct block_device *dev;
    uint64_t             block_no;
    uint8_t             *data;
    uint8_t              dirty;
    uint8_t              valid;
    uint8_t              _pad[2];
    uint64_t             last_used_tick;
};

static struct pcache_slot g_cache[PCACHE_MAX_ENTRIES];
static spinlock_t        g_pc_lock = SPINLOCK_INIT;
static uint64_t          g_tick = 0;
static uint32_t          g_hits = 0;
static uint32_t          g_misses = 0;
static int               g_inited = 0;

/* 每个 4KB 块 = 8 个扇区 */
#define PC_BLOCK_SECTORS  (PAGE_SIZE / BLOCK_SECTOR_SIZE)

void pcache_init(void)
{
    if (g_inited) return;
    for (int i = 0; i < PCACHE_MAX_ENTRIES; ++i) {
        g_cache[i].dev = 0;
        g_cache[i].block_no = 0;
        g_cache[i].data = 0;
        g_cache[i].dirty = 0;
        g_cache[i].valid = 0;
        g_cache[i].last_used_tick = 0;
    }
    g_tick = 0;
    g_hits = 0;
    g_misses = 0;
    spin_lock_init(&g_pc_lock);
    g_inited = 1;
    serial_printf("[PAGE-CACHE] init: entries=%u block=%u bytes\n",
                  (unsigned)PCACHE_MAX_ENTRIES, (unsigned)PAGE_SIZE);
}

/* 分配一个数据页（DMA 安全） */
static uint8_t *pc_alloc_data(void)
{
    void *va = block_dma_alloc(1);
    return (uint8_t *)va;
}

/* 调用者必须持有 g_pc_lock */
static struct pcache_slot *pc_find_locked(struct block_device *dev,
                                          uint64_t block_no)
{
    for (int i = 0; i < PCACHE_MAX_ENTRIES; ++i) {
        if (g_cache[i].valid && g_cache[i].dev == dev &&
            g_cache[i].block_no == block_no) {
            g_cache[i].last_used_tick = ++g_tick;
            return &g_cache[i];
        }
    }
    return 0;
}

/* 找一个空闲槽或 LRU 淘汰。
 * 调用者必须持有 g_pc_lock。
 * 返回淘汰槽（其 data 已释放时为 NULL）。 */
static struct pcache_slot *pc_evict_locked(void)
{
    int best = -1;
    uint64_t best_tick = ~0ULL;

    for (int i = 0; i < PCACHE_MAX_ENTRIES; ++i) {
        if (!g_cache[i].valid) return &g_cache[i];
        if (g_cache[i].dirty) continue;    /* 不淘汰脏页 */
        if (g_cache[i].last_used_tick < best_tick) {
            best_tick = g_cache[i].last_used_tick;
            best = i;
        }
    }
    if (best < 0) return 0;    /* 全脏，无法淘汰 */
    return &g_cache[best];
}

int pcache_get(struct block_device *dev, uint64_t block_no,
               uint8_t **out_data, int for_write)
{
    if (!dev || !out_data) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_pc_lock, &flags);

    struct pcache_slot *s = pc_find_locked(dev, block_no);
    if (s) {
        if (for_write) s->dirty = 1;
        g_hits++;
        *out_data = s->data;
        spin_unlock_irqrestore(&g_pc_lock, flags);
        return 0;
    }
    g_misses++;

    s = pc_evict_locked();
    if (!s) {
        spin_unlock_irqrestore(&g_pc_lock, flags);
        return OB_ENOMEM;
    }

    /* 若槽被占用（脏页已排除），先失效旧 data */
    if (s->data) {
        block_dma_free(s->data, 1);
        s->data = 0;
    }
    s->data = pc_alloc_data();
    if (!s->data) {
        s->valid = 0;
        spin_unlock_irqrestore(&g_pc_lock, flags);
        return OB_ENOMEM;
    }

    s->dev       = dev;
    s->block_no  = block_no;
    s->dirty     = 0;
    s->valid     = 1;
    s->last_used_tick = ++g_tick;

    spin_unlock_irqrestore(&g_pc_lock, flags);

    /* 从块层读取 */
    int rc = block_read(dev, block_no * PC_BLOCK_SECTORS,
                        PC_BLOCK_SECTORS, s->data);
    if (rc != 0) {
        uint64_t f2;
        spin_lock_irqsave(&g_pc_lock, &f2);
        s->valid = 0;
        spin_unlock_irqrestore(&g_pc_lock, f2);
        return rc;
    }

    if (for_write) {
        uint64_t f2;
        spin_lock_irqsave(&g_pc_lock, &f2);
        s->dirty = 1;
        spin_unlock_irqrestore(&g_pc_lock, f2);
    }

    *out_data = s->data;
    return 0;
}

int pcache_mark_dirty(struct block_device *dev, uint64_t block_no)
{
    if (!dev) return OB_EINVAL;
    uint64_t flags;
    spin_lock_irqsave(&g_pc_lock, &flags);
    struct pcache_slot *s = pc_find_locked(dev, block_no);
    if (s) s->dirty = 1;
    spin_unlock_irqrestore(&g_pc_lock, flags);
    return s ? 0 : OB_EIO;
}

int pcache_writeback(struct block_device *dev, uint64_t block_no)
{
    if (!dev) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_pc_lock, &flags);
    struct pcache_slot *s = pc_find_locked(dev, block_no);
    if (!s || !s->dirty) {
        spin_unlock_irqrestore(&g_pc_lock, flags);
        return 0;
    }
    uint8_t *data = s->data;
    s->dirty = 0;
    spin_unlock_irqrestore(&g_pc_lock, flags);

    return block_write(dev, block_no * PC_BLOCK_SECTORS,
                       PC_BLOCK_SECTORS, data);
}

int pcache_sync(void)
{
    int err = 0;
    for (int i = 0; i < PCACHE_MAX_ENTRIES; ++i) {
        uint64_t flags;
        spin_lock_irqsave(&g_pc_lock, &flags);
        if (!g_cache[i].valid || !g_cache[i].dirty) {
            spin_unlock_irqrestore(&g_pc_lock, flags);
            continue;
        }
        struct block_device *dev = g_cache[i].dev;
        uint64_t bno = g_cache[i].block_no;
        uint8_t *data = g_cache[i].data;
        g_cache[i].dirty = 0;
        spin_unlock_irqrestore(&g_pc_lock, flags);

        int rc = block_write(dev, bno * PC_BLOCK_SECTORS,
                             PC_BLOCK_SECTORS, data);
        if (rc != 0) err = rc;
    }
    return err;
}

uint32_t pcache_hit_count(void)  { return g_hits; }
uint32_t pcache_miss_count(void) { return g_misses; }

uint32_t pcache_dirty_count(void)
{
    uint32_t n = 0;
    uint64_t flags;
    spin_lock_irqsave(&g_pc_lock, &flags);
    for (int i = 0; i < PCACHE_MAX_ENTRIES; ++i)
        if (g_cache[i].valid && g_cache[i].dirty) n++;
    spin_unlock_irqrestore(&g_pc_lock, flags);
    return n;
}

void pcache_invalidate(struct block_device *dev, uint64_t block_no)
{
    uint64_t flags;
    spin_lock_irqsave(&g_pc_lock, &flags);
    struct pcache_slot *s = pc_find_locked(dev, block_no);
    if (s) {
        s->valid = 0;
        s->dirty = 0;
        if (s->data) {
            block_dma_free(s->data, 1);
            s->data = 0;
        }
    }
    spin_unlock_irqrestore(&g_pc_lock, flags);
}
/*===OmniBridgeOs/kernel/arch/x64/block/page_cache.c 结束===*/