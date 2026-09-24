/*===OmniBridgeOs/kernel/arch/x64/block/block.h===*/
/*
 * 通用块设备层（第 18B 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 块设备注册/注销在全局自旋锁保护下进行。
 *   - 每个块设备拥有独立的请求队列与自旋锁。
 *   - 扇区大小恒为 512 字节（VirtIO 约定）。
 *   - DMA 缓冲区必须由 pmm_alloc_pages 分配，通过 DirectMap 访问。
 *   - 块层不直接与 VFS 交互；页缓存是上层用户。
 */
#ifndef OMNIBRIDGE_BLOCK_BLOCK_H
#define OMNIBRIDGE_BLOCK_BLOCK_H

#include <stdint.h>
#include "spinlock.h"

/* ---------- 错误码 ---------- */
#ifndef OB_EIO
#define OB_EIO      (-5)
#endif
#ifndef OB_EINVAL
#define OB_EINVAL   (-22)
#endif
#ifndef OB_ENOMEM
#define OB_ENOMEM   (-12)
#endif
#ifndef OB_ENODEV
#define OB_ENODEV   (-19)
#endif
#ifndef OB_EAGAIN
#define OB_EAGAIN   (-11)
#endif

#define BLOCK_SECTOR_SIZE  512u
#define BLOCK_MAX_DEVICES  8

struct block_device;

/* 块设备的同步读写函数指针。
 * 返回值：0 成功；负错误码失败。
 *
 * 约定：buf 必须是与扇区大小对齐的物理页（pmm 分配 + DirectMap），
 *       count 为扇区数。 */
typedef int (*blk_read_fn)(struct block_device *dev, uint64_t lba,
                           uint32_t count, void *buf);
typedef int (*blk_write_fn)(struct block_device *dev, uint64_t lba,
                            uint32_t count, const void *buf);

struct block_device {
    char     name[16];
    uint32_t sector_size;       /* 恒为 512 */
    uint64_t total_sectors;     /* 总扇区数 */
    uint64_t start_lba;         /* 分区起始 LBA（整盘为 0） */

    blk_read_fn  read;
    blk_write_fn write;

    void    *driver_data;       /* 驱动私有指针 */
    uint8_t  registered;
    uint8_t  _pad[3];
    uint32_t index;             /* 全局索引 */

    spinlock_t lock;            /* 设备级 IO 串行化锁 */

    struct block_device *next;
};

/* 全局初始化（幂等）。 */
void block_init(void);

/* 注册一个块设备；成功返回 0。 */
int  block_register_device(struct block_device *dev);

/* 注销（用于卸载/测试）。 */
int  block_unregister_device(struct block_device *dev);

/* 按名称查找。 */
struct block_device *block_lookup(const char *name);

/* 遍历所有设备。 */
void block_iterate(void (*cb)(struct block_device *dev, void *arg), void *arg);

/* 设备数量。 */
uint32_t block_device_count(void);

/* 同步读写（扇区单位）。buf 必须为 DMA 安全内存。 */
int block_read(struct block_device *dev, uint64_t lba,
               uint32_t count, void *buf);
int block_write(struct block_device *dev, uint64_t lba,
                uint32_t count, const void *buf);

/* 分配/释放 DMA 安全的缓冲区（整页，PAGE_SIZE 对齐）。
 * 通过 DirectMap 虚拟地址返回。 */
void *block_dma_alloc(uint64_t pages);
void  block_dma_free(void *va, uint64_t pages);

#endif /* OMNIBRIDGE_BLOCK_BLOCK_H */
/*===OmniBridgeOs/kernel/arch/x64/block/block.h 结束===*/