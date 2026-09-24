/*===OmniBridgeOs/kernel/arch/x64/block/symlink.h===*/
#ifndef OMNIBRIDGE_BLOCK_SYMLINK_H
#define OMNIBRIDGE_BLOCK_SYMLINK_H

#include <stdint.h>

#define SYMLINK_MAX_TARGET  256

void symlink_init(void);

/* 校验目标路径：不允许以 /kernel、/system/kernel、/system/critical 开头。 */
int symlink_check_target(const char *target);

/* 解析符号链接：输入 link 内容与基准路径，输出合并后的绝对路径。
 * 返回 0 成功。-22 参数错误。-40 = -ELOOP。 */
int symlink_resolve(const char *link_target, const char *base_dir,
                    char *out, uint32_t out_size);

#endif /* OMNIBRIDGE_BLOCK_SYMLINK_H */
/*===OmniBridgeOs/kernel/arch/x64/block/symlink.h 结束===*/