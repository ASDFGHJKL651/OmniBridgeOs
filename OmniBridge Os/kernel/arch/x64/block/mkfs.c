/*===OmniBridgeOs/kernel/arch/x64/block/mkfs.c===*/
#include "mkfs.h"
#include "obfs_rw.h"
#include "../obfs.h"
#include "serial.h"
#include "block.h"
#include "pmm.h"

/*
 * ★ 修复：日志容量从 32 增到 256（1 MiB）。
 *   - 32 块日志仅够 31 个事务 entry。
 *   - 写 64KB 走 indirect1 时约需 40 个 entry（数据块 + 位图 + indirect 表 + inode），
 *     导致 journal_commit 返回 -ENOMEM。
 *   - 256 块足以支撑单次 256 KB 写入。
 */
#define MKFS_JOURNAL_SIZE   256u
#define MKFS_MIN_JOURNAL    128u
#define MKFS_DEFAULT_QUOTA  0ULL

static int write_block_direct(struct block_device *dev,
                              uint64_t block_no, const void *data)
{
    return block_write(dev, block_no * (PAGE_SIZE / 512),
                       PAGE_SIZE / 512, data);
}

static int read_block_direct(struct block_device *dev,
                             uint64_t block_no, void *buf)
{
    return block_read(dev, block_no * (PAGE_SIZE / 512),
                      PAGE_SIZE / 512, buf);
}

static void zero_bytes(void *p, uint64_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; ++i) b[i] = 0;
}

static uint64_t inode_table_blocks(uint64_t inode_count)
{
    uint64_t bytes = inode_count * OBFS_RW_INODE_SIZE;
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

/*
 * ★ 修复：obfs_is_formatted 现在也校验日志容量。
 *
 *   若磁盘上的 journal_size < MKFS_MIN_JOURNAL（旧镜像或损坏），
 *   本函数返回 0，让 main.c 触发重新 mkfs。
 *
 *   语义变化：
 *     - 老镜像（journal_size=32）→ 自动重格式化，旧数据丢失。
 *     - 这属于开发阶段的兼容策略，生产环境应改为显式升级工具。
 */
int obfs_is_formatted(struct block_device *dev, uint64_t start_block)
{
    if (!dev) return 0;

    uint8_t buf[PAGE_SIZE];
    if (read_block_direct(dev, start_block, buf) != 0) return 0;

    const struct obfs_superblock *sb =
        (const struct obfs_superblock *)buf;
    if (sb->magic != OBFS_MAGIC) return 0;
    if (sb->block_size != PAGE_SIZE) return 0;
    if (sb->total_blocks == 0) return 0;
    if (sb->inode_count == 0) return 0;
    if (sb->data_block_start >= sb->total_blocks) return 0;

    const struct obfs_superblock_rw *rw =
        (const struct obfs_superblock_rw *)buf;
    if (rw->rw_version != 1) return 0;
    if (rw->journal_size < MKFS_MIN_JOURNAL) return 0;   /* ★ 新增 */

    return 1;
}

int obfs_format(struct block_device *dev, uint64_t start_block,
                uint64_t total_blocks, uint64_t inode_count)
{
    if (!dev) return -22;
    if (total_blocks < 64) return -28;
    if (inode_count == 0 || inode_count > 4096) return -22;

    uint64_t T = inode_table_blocks(inode_count);
    uint64_t J = MKFS_JOURNAL_SIZE;

    uint64_t inode_table_start = 3;
    uint64_t journal_start     = inode_table_start + T;
    uint64_t data_block_start  = journal_start + J;

    if (data_block_start >= total_blocks) {
        serial_printf("[MKFS] image too small: need>=%llu blocks\n",
                      (unsigned long long)data_block_start + 1);
        return -28;
    }

    serial_printf("[MKFS] format start_block=%llu total_blocks=%llu "
                  "inode_count=%llu T=%llu J=%llu data_start=%llu\n",
                  (unsigned long long)start_block,
                  (unsigned long long)total_blocks,
                  (unsigned long long)inode_count,
                  (unsigned long long)T,
                  (unsigned long long)J,
                  (unsigned long long)data_block_start);

    uint8_t buf[PAGE_SIZE];

    /* ---- 1) 超级块 ---- */
    zero_bytes(buf, PAGE_SIZE);
    {
        struct obfs_superblock *sb = (struct obfs_superblock *)buf;
        sb->magic              = OBFS_MAGIC;
        sb->block_size         = PAGE_SIZE;
        sb->total_blocks       = total_blocks;
        sb->inode_count        = inode_count;
        sb->root_inode         = 0;
        sb->block_bitmap_start = 1;
        sb->inode_bitmap_start = 2;
        sb->inode_table_start  = inode_table_start;
        sb->data_block_start   = data_block_start;
        for (int i = 0; i < 32; ++i) sb->ita_public_key[i] = 0;

        struct obfs_superblock_rw *rw = (struct obfs_superblock_rw *)buf;
        rw->rw_version    = 1;
        rw->journal_start = (uint32_t)journal_start;
        rw->journal_size  = (uint32_t)J;
        rw->fsck_state    = 0;
        rw->default_quota = MKFS_DEFAULT_QUOTA;
        rw->crypto_algo   = 0;
        for (int i = 0; i < 7; ++i) rw->crypto_pad[i] = 0;
    }
    if (write_block_direct(dev, start_block + 0, buf) != 0) return -5;

    /* ---- 2) 块位图 ---- */
    zero_bytes(buf, PAGE_SIZE);
    for (uint64_t i = 0; i < data_block_start; ++i) {
        buf[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    if (write_block_direct(dev, start_block + 1, buf) != 0) return -5;

    /* ---- 3) inode 位图 ---- */
    zero_bytes(buf, PAGE_SIZE);
    buf[0] |= 0x01u;
    if (write_block_direct(dev, start_block + 2, buf) != 0) return -5;

    /* ---- 4) inode 表：先全部清零，再写根 inode ---- */
    for (uint64_t i = 0; i < T; ++i) {
        zero_bytes(buf, PAGE_SIZE);
        if (write_block_direct(dev, start_block + inode_table_start + i, buf)
            != 0) return -5;
    }

    zero_bytes(buf, PAGE_SIZE);
    {
        struct obfs_inode_rw *ino = (struct obfs_inode_rw *)buf;
        ino->base.mode   = (uint16_t)(VFS_S_IFDIR | 0755);
        ino->base.links  = 1;
        ino->base.size   = 0;
        ino->base.uid    = 0;
        ino->base.gid    = 0;
        ino->parent_ino  = 0;
        ino->link_count  = 1;
        ino->quota_limit = 0;
    }
    if (write_block_direct(dev, start_block + inode_table_start, buf)
        != 0) return -5;

    /* ---- 5) 日志区清零 ---- */
    for (uint64_t i = 0; i < J; ++i) {
        zero_bytes(buf, PAGE_SIZE);
        if (write_block_direct(dev, start_block + journal_start + i, buf)
            != 0) return -5;
    }

    serial_printf("[MKFS] format OK: %llu blocks, %llu inodes, "
                  "journal=%llu blocks at %llu\n",
                  (unsigned long long)total_blocks,
                  (unsigned long long)inode_count,
                  (unsigned long long)J,
                  (unsigned long long)journal_start);
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/block/mkfs.c 结束===*/