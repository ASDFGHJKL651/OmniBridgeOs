/*===OmniBridgeOs/kernel/arch/x64/obfs.h===*/
/*
 * OBFS 只读文件系统（第 11 步）。
 *
 * 磁盘布局（§10.1）：
 *   块 0        : 超级块（4K，其中有效字段 96 字节，其余清零）
 *   位图区      : 块位图 + inode 位图（本步只读，不写回）
 *   inode 表区  : 每 inode 256 字节，每块 16 个
 *   数据块区
 *
 * inode 布局（严格 256 字节）：
 *   mode/links/uid/gid/size/atime/mtime/ctime
 *   direct[12] + indirect1 + indirect2
 *   剩余保留
 *
 * 第 14 步修改：
 *   新增 ita_public_key[32] 的读写接口（内存镜像级别）。
 *   本步不会真正持久化（无 OBFS 可写挂载），仅供 OBFS 挂载场景下使用；
 *   第 14 步的 main.c 走 tmpfs 根，因此这两个接口在启动路径上不会被调用。
 *
 * 人工必须审查：
 *   - 所有偏移运算使用 uint64_t，防止溢出。
 *   - 边界检查：超级块内 total_blocks 必须 >= 各 region 起点；
 *     inode 号必须 < inode_count；块号必须 < total_blocks。
 *   - 只读：所有写操作返回 -EROFS。
 */
#ifndef OMNIBRIDGE_OBFS_H
#define OMNIBRIDGE_OBFS_H

#include <stdint.h>
#include "vfs.h"

#define OBFS_MAGIC       0x4F424653u  /* "OBFS" */
#define OBFS_BLOCK_SIZE  4096u
#define OBFS_INODE_SIZE  256u

/* ---------- 磁盘结构 ---------- */
struct obfs_superblock {
    uint32_t magic;               /* OBFS_MAGIC */
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t root_inode;
    uint8_t  ita_public_key[32];  /* 第 14 步：ITA 第二层公钥（Ed25519） */
    uint64_t block_bitmap_start;
    uint64_t inode_bitmap_start;
    uint64_t inode_table_start;
    uint64_t data_block_start;
    /* 其余填充到 OBFS_BLOCK_SIZE */
} __attribute__((packed));

struct obfs_inode {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t indirect1;
    uint32_t indirect2;
    uint8_t  reserved[156];       /* 填充到 256 字节 */
} __attribute__((packed));

/* 目录项（磁盘格式，80 字节定长） */
struct obfs_dirent {
    uint64_t ino;
    uint16_t type;                /* VFS_FT_* */
    uint16_t name_len;
    char     name[68];
} __attribute__((packed));

/* ---------- 接口 ---------- */

/* 从内存镜像挂载。返回 superblock 指针（调用方负责 vfs_mount），
 * 失败返回 NULL。 */
struct vfs_superblock *obfs_mount(const void *image, uint64_t size);

/* 卸载（由 vfs_umount 通过 ops->destroy_sb 调用）。 */
void obfs_umount(struct vfs_superblock *sb);

/* ---------- 第 14 步新增：ITA 公钥访问 ---------- */

/* 从 OBFS 超级块读取 ITA 公钥。成功返回 0。
 * 由于 OBFS 镜像为 const，本函数只做只读拷贝。 */
int obfs_get_ita_public_key(struct vfs_superblock *sb, uint8_t out[32]);

/* 写入 ITA 公钥。OBFS 是只读文件系统，本步返回 -EROFS。
 * 真正的持久化需要 OBFS 可写挂载（步骤 20+）。 */
int obfs_set_ita_public_key(struct vfs_superblock *sb,
                            const uint8_t pub[32]);

#endif /* OMNIBRIDGE_OBFS_H */