/*===OmniBridgeOs/kernel/arch/x64/block/fsck.c===*/
/*
 * OBFS fsck（第 18B 步 P1，修正版）。
 *
 * ★ 关键修正（对照上一版）：
 *   1) orphan 检查按磁盘位图正确索引：数据块在磁盘位图上的索引是
 *      data_block_start + i，而不是 i。原实现导致把全部元数据块
 *      （superblock/bitmaps/inode table/journal，共 data_block_start 个）
 *      误判为 orphan blocks。
 *   2) 文件 inode 的 indirect1 / indirect2 以及它们指向的数据块
 *      现在也纳入 reachable 集合。
 */
#include "fsck.h"
#include "page_cache.h"
#include "obfs_rw.h"
#include "serial.h"
#include "pmm.h"

#define FSCK_NDIRECT 12u
#define FSCK_NINDIR  (PAGE_SIZE / 4u)

void fsck_init(void)
{
    serial_printf("[FSCK] init\n");
}

struct fsck_bitmap {
    uint8_t *bits;
    uint64_t nbits;
    uint64_t count;
};

static inline int fsck_bm_test(struct fsck_bitmap *bm, uint64_t i)
{ return (bm->bits[i >> 3] >> (i & 7)) & 1; }

static inline void fsck_bm_set(struct fsck_bitmap *bm, uint64_t i)
{
    if (i >= bm->nbits) return;
    if (!fsck_bm_test(bm, i)) {
        bm->bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        bm->count++;
    }
}

static void fsck_bm_zero(struct fsck_bitmap *bm, uint8_t *storage,
                         uint64_t nbits)
{
    bm->bits  = storage;
    bm->nbits = nbits;
    bm->count = 0;
    for (uint64_t i = 0; i < (nbits + 7) / 8; ++i) storage[i] = 0;
}

/* 标记一个数据块（若在数据区内） */
static void mark_data_block(struct fsck_bitmap *block_seen,
                            uint64_t blk_abs, uint64_t data_start)
{
    if (blk_abs < data_start) return;
    fsck_bm_set(block_seen, blk_abs - data_start);
}

/* 遍历一个文件 inode 引用的全部数据块（direct + indirect1 + indirect2） */
static void mark_file_blocks(struct block_device *dev, uint64_t start_block,
                             const struct obfs_inode_rw *ino,
                             uint64_t data_start,
                             struct fsck_bitmap *block_seen)
{
    for (uint32_t j = 0; j < FSCK_NDIRECT; ++j) {
        mark_data_block(block_seen, ino->base.direct[j], data_start);
    }

    /* indirect1 */
    if (ino->base.indirect1) {
        mark_data_block(block_seen, ino->base.indirect1, data_start);
        uint8_t *ib = 0;
        if (pcache_get(dev, start_block + ino->base.indirect1, &ib, 0) == 0) {
            uint32_t *t = (uint32_t *)ib;
            for (uint32_t k = 0; k < FSCK_NINDIR; ++k) {
                if (t[k]) mark_data_block(block_seen, t[k], data_start);
            }
        }
    }

    /* indirect2 */
    if (ino->base.indirect2) {
        mark_data_block(block_seen, ino->base.indirect2, data_start);
        uint8_t *b1 = 0;
        if (pcache_get(dev, start_block + ino->base.indirect2, &b1, 0) == 0) {
            uint32_t *t1 = (uint32_t *)b1;
            for (uint32_t k = 0; k < FSCK_NINDIR; ++k) {
                if (t1[k] == 0) continue;
                mark_data_block(block_seen, t1[k], data_start);
                uint8_t *b2 = 0;
                if (pcache_get(dev, start_block + t1[k], &b2, 0) == 0) {
                    uint32_t *t2 = (uint32_t *)b2;
                    for (uint32_t m = 0; m < FSCK_NINDIR; ++m) {
                        if (t2[m]) mark_data_block(block_seen, t2[m],
                                                   data_start);
                    }
                }
            }
        }
    }
}

static void fsck_walk_dir(struct block_device *dev, uint64_t start_block,
                          const struct obfs_superblock *sb,
                          uint64_t dir_ino,
                          struct fsck_bitmap *inode_seen,
                          struct fsck_bitmap *block_seen,
                          int depth)
{
    if (depth > 16) return;
    if (dir_ino >= sb->inode_count) return;

    uint64_t ino_off = sb->inode_table_start * PAGE_SIZE +
                       dir_ino * OBFS_RW_INODE_SIZE;
    uint64_t blk_no  = ino_off / PAGE_SIZE;
    uint32_t in_blk  = (uint32_t)(ino_off % PAGE_SIZE);

    uint8_t *b = 0;
    if (pcache_get(dev, start_block + blk_no, &b, 0) != 0) return;

    struct obfs_inode_rw ino;
    {
        uint8_t *src = b + in_blk;
        uint8_t *dst = (uint8_t *)&ino;
        for (uint32_t i = 0; i < OBFS_RW_INODE_SIZE; ++i) dst[i] = src[i];
    }

    if ((ino.base.mode & VFS_S_IFMT) != VFS_S_IFDIR) return;

    for (uint32_t i = 0; i < FSCK_NDIRECT; ++i) {
        uint32_t dblk = ino.base.direct[i];
        if (dblk == 0) continue;

        mark_data_block(block_seen, dblk, sb->data_block_start);

        uint8_t *db = 0;
        if (pcache_get(dev, start_block + dblk, &db, 0) != 0) continue;

        for (uint32_t off = 0;
             off + sizeof(struct obfs_dirent) <= PAGE_SIZE;
             off += sizeof(struct obfs_dirent)) {
            struct obfs_dirent *de = (struct obfs_dirent *)(db + off);
            if (de->ino == 0) continue;
            if (de->ino >= sb->inode_count) continue;
            if (fsck_bm_test(inode_seen, de->ino)) continue;
            fsck_bm_set(inode_seen, de->ino);

            if (de->type == VFS_FT_DIR) {
                fsck_walk_dir(dev, start_block, sb, de->ino,
                              inode_seen, block_seen, depth + 1);
            } else {
                uint64_t cino_off = sb->inode_table_start * PAGE_SIZE +
                                    de->ino * OBFS_RW_INODE_SIZE;
                uint64_t cblk = cino_off / PAGE_SIZE;
                uint32_t cib = (uint32_t)(cino_off % PAGE_SIZE);
                uint8_t *cb = 0;
                if (pcache_get(dev, start_block + cblk, &cb, 0) != 0) continue;
                struct obfs_inode_rw cino;
                for (uint32_t k = 0; k < OBFS_RW_INODE_SIZE; ++k)
                    ((uint8_t *)&cino)[k] = cb[cib + k];
                mark_file_blocks(dev, start_block, &cino,
                                 sb->data_block_start, block_seen);
            }
        }
    }
}

int fsck_check(struct block_device *dev, uint64_t start_block,
               uint64_t total_blocks)
{
    if (!dev) return -22;

    uint8_t *blk0 = 0;
    if (pcache_get(dev, start_block, &blk0, 0) != 0) return -5;

    const struct obfs_superblock *sb = (const struct obfs_superblock *)blk0;
    if (sb->magic != OBFS_MAGIC) return -5;
    if (sb->block_size != PAGE_SIZE) return -5;
    if (sb->total_blocks > total_blocks) return -5;
    if (sb->inode_count == 0 || sb->inode_count > 4096) return -5;
    if (sb->root_inode >= sb->inode_count) return -5;
    if (sb->data_block_start >= sb->total_blocks) return -5;

    serial_printf("[FSCK] superblock OK: blocks=%llu inodes=%llu root=%llu\n",
                  (unsigned long long)sb->total_blocks,
                  (unsigned long long)sb->inode_count,
                  (unsigned long long)sb->root_inode);

    /* 静态存储避免大栈变量 */
    static uint8_t inode_seen_bits[512];
    static uint8_t block_seen_bits[PAGE_SIZE];

    struct fsck_bitmap inode_seen;
    struct fsck_bitmap block_seen;
    fsck_bm_zero(&inode_seen, inode_seen_bits, sb->inode_count);
    fsck_bm_zero(&block_seen, block_seen_bits,
                 sb->total_blocks - sb->data_block_start);

    fsck_bm_set(&inode_seen, sb->root_inode);

    fsck_walk_dir(dev, start_block, sb, sb->root_inode,
                  &inode_seen, &block_seen, 0);

    serial_printf("[FSCK] reachable inodes=%llu blocks=%llu\n",
                  (unsigned long long)inode_seen.count,
                  (unsigned long long)block_seen.count);

    /* inode 位图交叉校验 */
    uint8_t *ibm = 0;
    if (pcache_get(dev, start_block + sb->inode_bitmap_start, &ibm, 0) != 0)
        return -5;

    uint64_t orphan_inodes = 0;
    for (uint64_t i = 0; i < sb->inode_count; ++i) {
        int disk_bit = (ibm[i >> 3] >> (i & 7)) & 1;
        int reachable = fsck_bm_test(&inode_seen, i);
        if (disk_bit && !reachable) orphan_inodes++;
    }

    /* ★ 块位图交叉校验：磁盘索引 = data_block_start + i
     *   元数据块（0..data_block_start-1）由文件系统内部使用，
     *   不属于"数据块可达"检查范围。 */
    uint8_t *bm = 0;
    if (pcache_get(dev, start_block + sb->block_bitmap_start, &bm, 0) != 0)
        return -5;

    uint64_t orphan_blocks = 0;
    for (uint64_t i = 0; i < sb->total_blocks - sb->data_block_start; ++i) {
        uint64_t disk_idx = sb->data_block_start + i;
        int disk_bit = (bm[disk_idx >> 3] >> (disk_idx & 7)) & 1;
        int reachable = fsck_bm_test(&block_seen, i);
        if (disk_bit && !reachable) orphan_blocks++;
    }

    if (orphan_inodes || orphan_blocks) {
        serial_printf("[FSCK] WARN: orphan inodes=%llu blocks=%llu\n",
                      (unsigned long long)orphan_inodes,
                      (unsigned long long)orphan_blocks);
        return 1;
    }

    serial_printf("[FSCK] clean\n");
    return 0;
}

int fsck_run(struct block_device *dev, uint64_t start_block,
             uint64_t total_blocks, int repair)
{
    int rc = fsck_check(dev, start_block, total_blocks);
    if (rc < 0) return rc;
    if (rc == 0) return 0;
    if (!repair) return rc;

    serial_printf("[FSCK] repair requested (no-op in this step)\n");
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/block/fsck.c 结束===*/