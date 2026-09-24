/*===OmniBridgeOs/kernel/arch/x64/block/mkfs.h===*/
#ifndef OMNIBRIDGE_BLOCK_MKFS_H
#define OMNIBRIDGE_BLOCK_MKFS_H

#include <stdint.h>
#include "block.h"

/*
 * OBFS-RW 镜像格式化（第 18B 步 P0）。
 *
 * 磁盘布局（以 4K 块为单位，块号相对于 start_block）：
 *   block 0                        : superblock
 *   block 1                        : block bitmap
 *   block 2                        : inode bitmap
 *   block 3 .. 3+T-1               : inode table
 *   block 3+T .. 3+T+J-1           : journal (super + data)
 *   block 3+T+J .. total_blocks-1  : data blocks
 *
 *   其中：
 *     T = ceil(inode_count * OBFS_RW_INODE_SIZE / PAGE_SIZE)
 *     J = journal_size（默认 32）
 *
 * 初始化内容：
 *   - 超级块（含 rw 扩展字段：rw_version=1, fsck_state=0）
 *   - 块位图：前 (3+T+J) 块标记为已用；其余清零
 *   - inode 位图：仅 inode 0 置位
 *   - inode 表：全部清零；根 inode 0 置为目录
 *   - 日志头：magic=0（clean）
 *
 * 返回 0 成功；负错误码失败。
 */
int obfs_format(struct block_device *dev, uint64_t start_block,
                uint64_t total_blocks, uint64_t inode_count);

/* 探测超级块 magic 是否为 OBFS_MAGIC 且 rw_version==1。 */
int obfs_is_formatted(struct block_device *dev, uint64_t start_block);

#endif /* OMNIBRIDGE_BLOCK_MKFS_H */
/*===OmniBridgeOs/kernel/arch/x64/block/mkfs.h 结束===*/