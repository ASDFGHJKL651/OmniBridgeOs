/*===OmniBridgeOs/kernel/arch/x64/block/block_selftest.h===*/
#ifndef OMNIBRIDGE_BLOCK_SELFTEST_H
#define OMNIBRIDGE_BLOCK_SELFTEST_H

/* 块层 + OBFS 可写 + 页缓存 + 日志 + 配额 + xattr + symlink 综合自检。
 * 在无 VirtIO 块设备时退化为仅检查接口可用性。 */
void block_selftest(void);

#endif /* OMNIBRIDGE_BLOCK_SELFTEST_H */
/*===OmniBridgeOs/kernel/arch/x64/block/block_selftest.h 结束===*/