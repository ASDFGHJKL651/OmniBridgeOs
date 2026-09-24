/*===OmniBridgeOs/kernel/arch/x64/block/obfs_rw.h===*/
#ifndef OMNIBRIDGE_BLOCK_OBFS_RW_H
#define OMNIBRIDGE_BLOCK_OBFS_RW_H

#include <stdint.h>
#include "block.h"
#include "journal.h"
#include "../obfs.h"
#include "../vfs.h"

/*
 * 可写 OBFS 磁盘 inode（严格 384 字节）。
 *
 * 与只读 OBFS 的 256 字节 inode 兼容：前 256 字节布局完全一致；
 * rw 版本把 inode 表项大小扩展为 384 字节，追加配额/硬链接/保留字段。
 */
#define OBFS_RW_INODE_SIZE  384

struct obfs_inode_rw {
    struct obfs_inode base;         /* 256 字节 */
    uint64_t quota_limit;           /* +8  = 264 */
    uint64_t parent_ino;            /* +8  = 272 */
    uint32_t link_count;            /* +4  = 276 */
    uint32_t _pad0;                 /* +4  = 280 */
    uint8_t  reserved[104];         /* +104= 384 */
} __attribute__((packed));

/*
 * VFS inode 的内存包装。
 *
 * ★ 第 18B 步修正：从 obfs_rw.c 内部提升到头文件。
 *   原因：block_selftest.c / oshell.c 需要访问 disk 字段（配额、链接计数等），
 *   仅在 .c 中定义会导致编译期 incomplete type 错误。
 */
struct obfs_rw_inode_wrap {
    struct obfs_inode_rw disk;
};

/* 可写超级块扩展字段（紧跟只读超级块之后）。 */
struct obfs_superblock_rw {
    struct obfs_superblock base;    /* 96 字节 */
    uint32_t rw_version;            /* 1 = 可写 */
    uint32_t journal_start;         /* 4K 块号（相对 start_block） */
    uint32_t journal_size;          /* 日志块数 */
    uint32_t fsck_state;            /* 0=clean, 1=dirty */
    uint64_t default_quota;
    uint8_t  crypto_algo;
    uint8_t  crypto_pad[7];
} __attribute__((packed));

/* 挂载可写 OBFS。若 rw_version==1 则 writable=1。 */
struct vfs_superblock *obfs_rw_mount(struct block_device *dev,
                                     uint64_t start_block,
                                     uint64_t total_blocks);

void obfs_rw_umount(struct vfs_superblock *sb);

/* 配额辅助。 */
uint64_t obfs_inode_get_quota(const struct obfs_inode *ino);
int      obfs_inode_set_quota(struct obfs_inode *ino, uint64_t limit);

/* 可写判定。 */
int obfs_rw_is_writable(struct vfs_superblock *sb);

/* 访问内部日志句柄（fsck、oshell sync 使用）。 */
struct journal *obfs_rw_journal(struct vfs_superblock *sb);

/* ★ 访问器：从 VFS inode 拿到磁盘 rw inode 指针。
 *   vi->fs_data 必须由本模块创建（即 vi->sb->ops 为 obfs_rw 的 ops）。
 *   返回 NULL 表示参数无效。 */
struct obfs_inode_rw *obfs_rw_disk_inode(struct vfs_inode *vi);

/* 硬链接：在 dir 下为 existing 创建一个名为 name 的新目录项。 */
int obfs_rw_link(struct vfs_inode *dir, const char *name,
                 struct vfs_inode *existing);

/* 符号链接：在 dir 下创建名为 name 的符号链接，指向 target。 */
int obfs_rw_symlink(struct vfs_inode *dir, const char *name,
                    const char *target);

/* 读符号链接。 */
int obfs_rw_readlink(struct vfs_inode *ino, char *buf, uint32_t bufsz);

/* 手动同步。 */
int obfs_rw_sync(struct vfs_superblock *sb);

#endif /* OMNIBRIDGE_BLOCK_OBFS_RW_H */
/*===OmniBridgeOs/kernel/arch/x64/block/obfs_rw.h 结束===*/