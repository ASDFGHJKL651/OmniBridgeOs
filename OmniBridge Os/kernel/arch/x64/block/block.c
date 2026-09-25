/*===OmniBridgeOs/kernel/arch/x64/block/block.c===*/
#include "block.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "printk.h"

static struct block_device *g_devices = 0;
static spinlock_t           g_block_lock = SPINLOCK_INIT;
static uint32_t             g_device_count = 0;
static uint32_t             g_next_index = 0;
static int                  g_block_inited = 0;

void block_init(void)
{
    if (g_block_inited) return;
    g_block_inited = 1;
    g_devices = 0;
    g_device_count = 0;
    g_next_index = 0;
    spin_lock_init(&g_block_lock);
    serial_printf("[BLOCK] init\n");
}

int block_register_device(struct block_device *dev)
{
    if (!dev) return OB_EINVAL;
    if (!dev->read || !dev->write) return OB_EINVAL;
    if (dev->sector_size != BLOCK_SECTOR_SIZE) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_block_lock, &flags);

    if (g_device_count >= BLOCK_MAX_DEVICES) {
        spin_unlock_irqrestore(&g_block_lock, flags);
        return OB_ENOMEM;
    }

    /* 重名检查 */
    for (struct block_device *d = g_devices; d; d = d->next) {
        if (d->name[0] == dev->name[0]) {
            int eq = 1;
            for (int i = 0; i < 16; ++i) {
                if (d->name[i] != dev->name[i]) { eq = 0; break; }
                if (d->name[i] == '\0') break;
            }
            if (eq) {
                spin_unlock_irqrestore(&g_block_lock, flags);
                return -17; /* -EEXIST */
            }
        }
    }

    dev->index = g_next_index++;
    dev->registered = 1;
    spin_lock_init(&dev->lock);
    dev->next = g_devices;
    g_devices = dev;
    g_device_count++;

    spin_unlock_irqrestore(&g_block_lock, flags);

    serial_printf("[BLOCK] device registered: %s sectors=%llu "
                  "sector_size=%u idx=%u\n",
                  dev->name,
                  (unsigned long long)dev->total_sectors,
                  (unsigned)dev->sector_size,
                  (unsigned)dev->index);
    return 0;
}

int block_unregister_device(struct block_device *dev)
{
    if (!dev) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_block_lock, &flags);

    struct block_device **pp = &g_devices;
    while (*pp && *pp != dev) pp = &(*pp)->next;
    if (!*pp) {
        spin_unlock_irqrestore(&g_block_lock, flags);
        return OB_ENODEV;
    }
    *pp = dev->next;
    dev->next = 0;
    dev->registered = 0;
    if (g_device_count > 0) g_device_count--;

    spin_unlock_irqrestore(&g_block_lock, flags);
    return 0;
}

struct block_device *block_lookup(const char *name)
{
    if (!name) return 0;

    uint64_t flags;
    spin_lock_irqsave(&g_block_lock, &flags);

    struct block_device *found = 0;
    for (struct block_device *d = g_devices; d; d = d->next) {
        int eq = 1;
        for (int i = 0; i < 16; ++i) {
            if (d->name[i] != name[i]) { eq = 0; break; }
            if (d->name[i] == '\0') break;
        }
        if (eq) { found = d; break; }
    }

    spin_unlock_irqrestore(&g_block_lock, flags);
    return found;
}

void block_iterate(void (*cb)(struct block_device *dev, void *arg), void *arg)
{
    if (!cb) return;

    uint64_t flags;
    spin_lock_irqsave(&g_block_lock, &flags);
    for (struct block_device *d = g_devices; d; d = d->next) {
        cb(d, arg);
    }
    spin_unlock_irqrestore(&g_block_lock, flags);
}

uint32_t block_device_count(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_block_lock, &flags);
    uint32_t n = g_device_count;
    spin_unlock_irqrestore(&g_block_lock, flags);
    return n;
}

/* ============================================================
 * 同步读写
 *
 * 人工必须审查：
 *   - 设备级 lock 串行化所有对该设备的 IO。
 *   - 调用驱动的 read/write 时不持锁，避免驱动内部再取锁造成死锁。
 *     （当前所有驱动都是同步的，实际上不会并发。）
 * ============================================================ */

int block_read(struct block_device *dev, uint64_t lba,
               uint32_t count, void *buf)
{
    if (!dev || !dev->read || !buf || count == 0) return OB_EINVAL;
    if (lba + count > dev->total_sectors) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&dev->lock, &flags);
    int rc = dev->read(dev, lba, count, buf);
    spin_unlock_irqrestore(&dev->lock, flags);
    return rc;
}

int block_write(struct block_device *dev, uint64_t lba,
                uint32_t count, const void *buf)
{
    if (!dev || !dev->write || !buf || count == 0) return OB_EINVAL;
    if (lba + count > dev->total_sectors) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&dev->lock, &flags);
    int rc = dev->write(dev, lba, count, buf);
    spin_unlock_irqrestore(&dev->lock, flags);
    return rc;
}

/* ============================================================
 * DMA 缓冲区
 *
 * 人工必须审查：
 *   - 必须由 pmm_alloc_pages 分配物理连续页。
 *   - 返回 DirectMap 虚拟地址，驱动通过该地址读写。
 *   - 不允许与 kmalloc（SLAB）混用。
 * ============================================================ */

void *block_dma_alloc(uint64_t pages)
{
    if (pages == 0 || pages > (1ULL << MAX_ORDER)) return 0;

    int order = 0;
    while (((uint64_t)1 << order) < pages) order++;
    if (order >= MAX_ORDER) return 0;

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) return 0;

    uint64_t pa = page_to_phys(pg);
    void *va = (void *)(uintptr_t)(DIRECTMAP_BASE + pa);

    uint8_t *p = (uint8_t *)va;
    uint64_t bytes = PAGE_SIZE << order;
    for (uint64_t i = 0; i < bytes; ++i) p[i] = 0;

    return va;
}

void block_dma_free(void *va, uint64_t pages)
{
    if (!va || pages == 0) return;

    int order = 0;
    while (((uint64_t)1 << order) < pages) order++;
    if (order >= MAX_ORDER) return;

    uint64_t vaddr = (uint64_t)(uintptr_t)va;
    uint64_t pa = vaddr - DIRECTMAP_BASE;
    struct page *pg = phys_to_page(pa);
    pmm_free_pages(pg, order);
}
/*===OmniBridgeOs/kernel/arch/x64/block/block.c 结束===*/