/*===OmniBridgeOs/kernel/arch/x64/block/fsck.h===*/
#ifndef OMNIBRIDGE_BLOCK_FSCK_H
#define OMNIBRIDGE_BLOCK_FSCK_H

#include "block.h"

/*
 * 简化 fsck：
 *   - 校验超级块 magic/block_size/各 region 范围。
 *   - 校验 inode 位图与目录项引用是否一致。
 *   - 校验块位图与 inode direct 数组引用是否一致。
 *   - 对不一致的引用，可以自动修复（清 inode 位图、释放块）。
 */
void fsck_init(void);

/* 运行 fsck。
 *   dev / start_block / total_blocks  —— 目标设备与范围
 *   repair                            —— 非 0 时执行修复
 * 返回 0 表示干净或已修复；负错误码表示无法修复。 */
int fsck_run(struct block_device *dev, uint64_t start_block,
             uint64_t total_blocks, int repair);

/* 仅检查，不修复。 */
int fsck_check(struct block_device *dev, uint64_t start_block,
               uint64_t total_blocks);

#endif /* OMNIBRIDGE_BLOCK_FSCK_H */
/*===OmniBridgeOs/kernel/arch/x64/block/fsck.h 结束===*/