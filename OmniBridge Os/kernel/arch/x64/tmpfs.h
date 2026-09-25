/*===OmniBridgeOs/kernel/arch/x64/tmpfs.h===*/
#ifndef OMNIBRIDGE_TMPFS_H
#define OMNIBRIDGE_TMPFS_H

#include "vfs.h"

#define TMPFS_MAGIC  0x746D7073u   /* "tmpfs" */

/*
 * 挂载一个全新的 tmpfs。
 *   name     : 调试名
 *   max_size : 单实例最大字节数；0 表示默认（64 MiB）
 * 返回 superblock 指针；失败返回 NULL。
 *
 * 人工必须审查：
 *   - 返回的 sb 由 tmpfs 内部持有（root inode + fs_data + sb 本身），
 *     需通过 vfs_umount() 或 tmpfs_umount() 释放，绝不能直接 kfree(sb)。
 */
struct vfs_superblock *tmpfs_mount(const char *name, uint64_t max_size);

/* 直接销毁一个未挂载的 tmpfs（等价于 vfs_umount 的底层动作）。 */
void tmpfs_umount(struct vfs_superblock *sb);

/*
 * 为权限 0 进程创建隔离内存树：根下仅 /bin、/tmp、/home 三个空目录。
 * 成功后 *out 指向 sb（未挂载到 VFS）。调用方负责后续挂载或释放。
 */
int tmpfs_create_priv0_tree(struct vfs_superblock **out);

/*
 * 为权限 1 进程在给定 tmpfs 上创建 /tmp/priv1_<pid>_<suffix>/。
 *   out_path / out_size : 输出形如 "/tmp/priv1_42_a/" 的完整路径。
 *
 * 说明（本步）：
 *   suffix 目前固定为单字符 'a'；随机后缀在步骤 15 引入。
 *   同一 tmpfs 上对同一 pid 重复调用会因目录已存在而返回 OB_EEXIST。
 */
int tmpfs_create_priv1_dir(struct vfs_superblock *sb, uint64_t pid,
                           char *out_path, uint64_t out_size);

#endif /* OMNIBRIDGE_TMPFS_H */
/*===OmniBridgeOs/kernel/arch/x64/tmpfs.h 结束===*/