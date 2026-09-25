/*===OmniBridgeOs/kernel/arch/x64/vfs.h===*/
#ifndef OMNIBRIDGE_VFS_H
#define OMNIBRIDGE_VFS_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

/* ============================================================
 * 名称 / 路径长度上限
 * ============================================================ */
#define VFS_NAME_MAX  256
#define VFS_PATH_MAX  512

/* ============================================================
 * VFS 错误码（每个常量单独 #ifndef）
 * ============================================================ */
#ifndef OB_EPERM
#define OB_EPERM     (-1)
#endif
#ifndef OB_ENOENT
#define OB_ENOENT    (-2)
#endif
#ifndef OB_EIO
#define OB_EIO       (-5)
#endif
#ifndef OB_EBADF
#define OB_EBADF     (-9)
#endif
#ifndef OB_EAGAIN
#define OB_EAGAIN    (-11)
#endif
#ifndef OB_ENOMEM
#define OB_ENOMEM    (-12)
#endif
#ifndef OB_EACCES
#define OB_EACCES    (-13)
#endif
#ifndef OB_EEXIST
#define OB_EEXIST    (-17)
#endif
#ifndef OB_ENOTDIR
#define OB_ENOTDIR   (-20)
#endif
#ifndef OB_EISDIR
#define OB_EISDIR    (-21)
#endif
#ifndef OB_EINVAL
#define OB_EINVAL    (-22)
#endif
#ifndef OB_EROFS
#define OB_EROFS     (-30)
#endif
#ifndef OB_ENOSYS
#define OB_ENOSYS    (-38)
#endif
#ifndef OB_ENOTEMPTY
#define OB_ENOTEMPTY (-39)
#endif

/* ============================================================
 * 文件类型（mode 高位）与访问模式
 * ============================================================ */
#define VFS_S_IFMT    0xF000u
#define VFS_S_IFSOCK  0xC000u
#define VFS_S_IFLNK   0xA000u
#define VFS_S_IFREG   0x8000u
#define VFS_S_IFBLK   0x6000u
#define VFS_S_IFDIR   0x4000u
#define VFS_S_IFCHR   0x2000u
#define VFS_S_IFIFO   0x1000u

#define VFS_S_ISREG(m) (((m) & VFS_S_IFMT) == VFS_S_IFREG)
#define VFS_S_ISDIR(m) (((m) & VFS_S_IFMT) == VFS_S_IFDIR)
#define VFS_S_ISLNK(m) (((m) & VFS_S_IFMT) == VFS_S_IFLNK)

/* ============================================================
 * 目录项类型（vfs_dirent.type）
 *
 * 同时提供 VFS_DT_* 与 VFS_FT_* 两套名称，数值一致。
 * ============================================================ */
#define VFS_DT_UNKNOWN  0
#define VFS_DT_REG      1
#define VFS_DT_DIR      2
#define VFS_DT_CHR      3
#define VFS_DT_BLK      4
#define VFS_DT_FIFO     5
#define VFS_DT_SOCK     6
#define VFS_DT_LNK      7

#define VFS_FT_UNKNOWN  VFS_DT_UNKNOWN
#define VFS_FT_REG      VFS_DT_REG
#define VFS_FT_DIR      VFS_DT_DIR
#define VFS_FT_CHR      VFS_DT_CHR
#define VFS_FT_BLK      VFS_DT_BLK
#define VFS_FT_FIFO     VFS_DT_FIFO
#define VFS_FT_SOCK     VFS_DT_SOCK
#define VFS_FT_LNK      VFS_DT_LNK

/* 打开标志位 */
#define VFS_O_RDONLY  0x0001
#define VFS_O_WRONLY  0x0002
#define VFS_O_RDWR    (VFS_O_RDONLY | VFS_O_WRONLY)
#define VFS_O_CREAT   0x0010
#define VFS_O_EXCL    0x0020
#define VFS_O_TRUNC   0x0040
#define VFS_O_APPEND  0x0080

/* 路径解析 flags —— 保留位 */
#define VFS_LOOKUP_FOLLOW   0x01
#define VFS_LOOKUP_PARENT   0x02

/* ============================================================
 * 目录项
 * ============================================================ */
struct vfs_dirent {
    uint64_t ino;
    uint8_t  type;
    uint8_t  _pad[3];
    uint32_t name_len;
    char     name[VFS_NAME_MAX];
};

/* 前向声明 */
struct vfs_inode;
struct vfs_dentry;
struct vfs_superblock;
struct vfs_file;
struct vfs_operations;

/* ============================================================
 * inode
 * ============================================================ */
struct vfs_inode {
    uint64_t ino;
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t flags;
    uint32_t _pad;

    struct vfs_superblock       *sb;
    const struct vfs_operations *ops;

    void *fs_data;

    uint32_t refcount;
    uint32_t _pad2;
};

/* ============================================================
 * dentry —— 保留定义，本步 VFS 层不使用
 *
 * 说明：本步路径解析不建立持久化的 dentry 缓存；
 *       定义保留供步骤 13 引入 dcache 时启用。
 * ============================================================ */
struct vfs_dentry {
    char name[VFS_NAME_MAX];
    uint32_t name_len;
    uint32_t _pad;
    struct vfs_inode      *inode;
    struct vfs_dentry     *parent;
    struct vfs_dentry     *children;
    struct vfs_dentry     *sibling_next;
    struct vfs_superblock *sb;
};

/* ============================================================
 * superblock
 * ============================================================ */
struct vfs_superblock {
    uint32_t magic;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t free_blocks;

    struct vfs_inode            *root;
    const struct vfs_operations *ops;
    void *fs_data;

    struct vfs_superblock *next;
    char mount_path[VFS_PATH_MAX];

    spinlock_t lock;
};

/* ============================================================
 * 打开的文件
 * ============================================================ */
struct vfs_file {
    uint64_t f_pos;
    uint32_t f_flags;
    uint32_t _pad;

    struct vfs_inode *f_inode;
    void *private_data;

    uint32_t refcount;
    uint32_t _pad2;
};

/* ============================================================
 * 操作表
 * ============================================================ */
struct vfs_operations {
    int (*lookup)(struct vfs_inode *dir, const char *name,
                  struct vfs_inode **out_inode);

    int (*create)(struct vfs_inode *dir, const char *name,
                  uint16_t mode, struct vfs_inode **out);

    int (*unlink)(struct vfs_inode *dir, const char *name);

    int (*mkdir)(struct vfs_inode *dir, const char *name,
                 uint16_t mode, struct vfs_inode **out);

    int (*rmdir)(struct vfs_inode *dir, const char *name);

    int64_t (*read)(struct vfs_file *f, void *buf, uint64_t count);
    int64_t (*write)(struct vfs_file *f, const void *buf, uint64_t count);

    int (*readdir)(struct vfs_inode *dir, uint64_t index,
                   struct vfs_dirent *out);

    int (*open)(struct vfs_inode *inode, struct vfs_file *file);
    int (*close)(struct vfs_file *file);

    int (*truncate)(struct vfs_inode *inode, uint64_t size);

    void (*evict_inode)(struct vfs_inode *inode);
    void (*destroy_sb)(struct vfs_superblock *sb);
    /* ★ 第 18D 步任务 5：poll 回调。
     * 返回就绪事件位（POLLIN/POLLOUT/POLLERR/POLLHUP）。
     * 可为 NULL，表示"始终就绪"。 */
    int (*poll)(struct vfs_file *f, short events);
};

/* ============================================================
 * VFS 核心接口
 * ============================================================ */
void vfs_init(void);

int vfs_mount(const char *path, struct vfs_superblock *sb);
int vfs_umount(const char *path);

uint32_t vfs_mount_count(void);
int      vfs_is_mounted(const char *path);

/* ★ 路径解析：返回借用 inode 指针，生命周期由 FS 层管理，
 *   调用方**不负责**释放。
 *   语义：返回 0 时 *out 指向路径对应的 vfs_inode；
 *        返回负错误码时 *out 置 NULL。 */
int vfs_lookup(const char *path, struct vfs_inode **out);

int      vfs_open(const char *path, uint32_t flags, struct vfs_file **out);
int64_t  vfs_read(struct vfs_file *f, void *buf, uint64_t count);
int64_t  vfs_write(struct vfs_file *f, const void *buf, uint64_t count);
int      vfs_close(struct vfs_file *f);

int vfs_mkdir(const char *path, uint16_t mode);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);

/* ★ 目录遍历：第一参为 inode 指针，index 为条目序号（从 0 开始） */
int vfs_readdir(struct vfs_inode *dir, uint64_t index,
                struct vfs_dirent *out);

/* ============================================================
 * 便捷内联
 * ============================================================ */
static inline uint8_t vfs_mode_to_dt(uint16_t mode)
{
    switch (mode & VFS_S_IFMT) {
    case VFS_S_IFREG:  return VFS_FT_REG;
    case VFS_S_IFDIR:  return VFS_FT_DIR;
    case VFS_S_IFLNK:  return VFS_FT_LNK;
    case VFS_S_IFCHR:  return VFS_FT_CHR;
    case VFS_S_IFBLK:  return VFS_FT_BLK;
    case VFS_S_IFIFO:  return VFS_FT_FIFO;
    case VFS_S_IFSOCK: return VFS_FT_SOCK;
    default:           return VFS_FT_UNKNOWN;
    }
}

static inline uint64_t vfs_file_pos(const struct vfs_file *f)
{
    return f ? f->f_pos : 0;
}

static inline void vfs_file_seek(struct vfs_file *f, uint64_t pos)
{
    if (f) f->f_pos = pos;
}

#endif /* OMNIBRIDGE_VFS_H */
/*===OmniBridgeOs/kernel/arch/x64/vfs.h 结束===*/