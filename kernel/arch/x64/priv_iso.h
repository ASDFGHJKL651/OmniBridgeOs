/*===OmniBridgeOs/kernel/arch/x64/priv_iso.h===*/
#ifndef OMNIBRIDGE_PRIV_ISO_H
#define OMNIBRIDGE_PRIV_ISO_H

#include <stdint.h>
#include <stddef.h>
#include "task.h"

/*
 * 权限 0/1 的文件系统与临时目录隔离（第 15 步）。
 *
 * 权限 0（VFS-in-RAM）：
 *   - 每个进程在根 tmpfs 上获得 /priv0/<pid>/ 挂载点；
 *   - 该挂载点下是独立的 1 MiB tmpfs 实例；
 *   - 进程终止时同步卸载。
 *
 * 权限 1（临时专用目录）：
 *   - 路径形式：/tmp/priv1_<pid>_<8字符随机后缀>/；
 *   - 该路径（含结尾 '/'）的 SHA-384 前 32 字节写入
 *     t->security_token.dir_whitelist_hash；
 *   - 完整路径记录在 t->priv1_dir_path（不含结尾 '/'）。
 */

/* 权限 0：构造绝对路径前缀（不含结尾 '/'），形如 "/priv0/1234"。
 * 成功返回 0；缓冲不足返回 OB_EINVAL。 */
int priv0_mount_path(uint64_t pid, char *out, size_t out_size);

/* 权限 0：在根 tmpfs 下创建 /priv0/<pid>/ 并挂载独立 tmpfs。
 * 幂等：已挂载则直接返回 0。 */
int priv0_mount_vfs(struct task_t *t);

/* 权限 0：卸载并删除 /priv0/<pid>/。忽略清理过程中的错误。 */
void priv0_umount_vfs(struct task_t *t);

/* 权限 1：创建 /tmp/priv1_<pid>_<suffix>/ 并计算 dir_whitelist_hash。
 * 若目录已存在则视为复用（返回 0）。 */
int priv1_setup_tmpdir(struct task_t *t);

/* 权限 1：删除临时目录。忽略错误（退出路径不可阻塞）。 */
void priv1_cleanup_tmpdir(struct task_t *t);

/* 权限 1：path 是否落在本进程的临时目录内。
 * 语义：字符串前缀匹配 + 常量时间 hash 一致性检查。
 * 返回 1 命中；0 不命中。 */
int priv1_path_allowed(const struct task_t *t, const char *path);

/* 计算路径（不含 NUL）的 SHA-384 到 out_hash[48]。
 * 若任一参数为 NULL 则不做任何事。 */
void priv_iso_path_hash(const char *path, uint8_t out_hash[48]);

#endif /* OMNIBRIDGE_PRIV_ISO_H */
/*===OmniBridgeOs/kernel/arch/x64/priv_iso.h 结束===*/