/*===OmniBridgeOs/kernel/arch/x64/obfs.c===*/
#include "obfs.h"
#include "kmalloc.h"
#include "serial.h"
#include "task.h"

/*
 * OBFS 只读实现。
 *
 * 关键不变量（人工必须审查）：
 *   - 所有块号访问前都必须经过 block_in_range() 校验。
 *   - inode 号必须经过 ino_in_range() 校验。
 *   - read 数据路径上禁止任何写操作。
 *   - 只读语义：create/mkdir/unlink/rmdir/write 全部返回 -EROFS。
 *
 * 第 14 步修改：
 *   - 新增 obfs_get_ita_public_key() / obfs_set_ita_public_key()。
 *   - 由于 OBFS 镜像为 const，写入返回 -EROFS，属于预期行为。
 */

/* ---------- 私有结构 ---------- */

struct obfs_sb_info {
    const uint8_t *image;
    uint64_t       image_size;
    uint64_t       total_blocks;
    uint64_t       block_size;
    uint64_t       inode_count;
    uint64_t       inode_table_start;
    uint64_t       data_block_start;
};

struct obfs_inode_wrap {
    struct obfs_inode disk;
};

/* ---------- 工具 ---------- */

static int block_in_range(struct obfs_sb_info *si, uint64_t blk)
{
    return blk < si->total_blocks;
}

static int ino_in_range(struct obfs_sb_info *si, uint64_t ino)
{
    return ino < si->inode_count;
}

/* 计算 inode 在镜像中的字节偏移 */
static uint64_t inode_offset(struct obfs_sb_info *si, uint64_t ino)
{
    return si->inode_table_start * si->block_size
         + ino * OBFS_INODE_SIZE;
}

/* 读一个数据块到 out（调用者保证 out 至少 block_size 字节） */
static int read_block(struct obfs_sb_info *si, uint64_t blk, void *out)
{
    if (!block_in_range(si, blk)) return OB_EIO;
    uint64_t off = blk * si->block_size;
    if (off + si->block_size > si->image_size) return OB_EIO;
    const uint8_t *src = si->image + off;
    uint8_t *dst = (uint8_t *)out;
    for (uint64_t i = 0; i < si->block_size; ++i) dst[i] = src[i];
    return 0;
}

/* ---------- inode 层 ---------- */

/* 从磁盘读 inode 到内存结构。返回 0 成功。 */
static int read_inode(struct obfs_sb_info *si, uint64_t ino,
                      struct obfs_inode *out)
{
    if (!ino_in_range(si, ino)) return OB_EIO;
    uint64_t off = inode_offset(si, ino);
    if (off + OBFS_INODE_SIZE > si->image_size) return OB_EIO;
    const uint8_t *src = si->image + off;
    uint8_t *dst = (uint8_t *)out;
    for (uint64_t i = 0; i < OBFS_INODE_SIZE; ++i) dst[i] = src[i];
    return 0;
}

/* 将磁盘 inode 号解析为一个 vfs_inode。
 * 返回 0 成功；失败返回负错误码。out 指向新分配的 vfs_inode（含
 * fs_data 为 obfs_inode_wrap*）。 */
static int obfs_get_inode(struct vfs_superblock *sb, uint64_t ino,
                          struct vfs_inode **out)
{
    struct obfs_sb_info *si = (struct obfs_sb_info *)sb->fs_data;
    struct vfs_inode *vi = (struct vfs_inode *)kzalloc(sizeof(*vi));
    if (!vi) return OB_EAGAIN;
    struct obfs_inode_wrap *wrap =
        (struct obfs_inode_wrap *)kzalloc(sizeof(*wrap));
    if (!wrap) {
        kfree(vi);
        return OB_EAGAIN;
    }

    int rc = read_inode(si, ino, &wrap->disk);
    if (rc != 0) {
        kfree(wrap);
        kfree(vi);
        return rc;
    }

    vi->ino    = ino;
    vi->mode   = wrap->disk.mode;
    vi->size   = wrap->disk.size;
    vi->atime  = wrap->disk.atime;
    vi->mtime  = wrap->disk.mtime;
    vi->ctime  = wrap->disk.ctime;
    vi->sb     = sb;
    vi->ops    = sb->ops;
    vi->fs_data = wrap;

    *out = vi;
    return 0;
}

/* evict_inode 回调 */
static void obfs_evict_inode(struct vfs_inode *inode)
{
    if (!inode) return;
    if (inode->fs_data) {
        kfree(inode->fs_data);
        inode->fs_data = 0;
    }
    kfree(inode);
}

/* ---------- 数据读取 ---------- */

/* 解析逻辑块索引 -> 物理块号。
 * 返回 0 成功；-1 表示该逻辑块不存在（稀疏，仅见于文件空洞，
 * 本步视为全零块）。 */
static int map_block(struct obfs_sb_info *si, struct obfs_inode *di,
                     uint64_t logical, uint32_t *out_blk)
{
    uint32_t per_block = (uint32_t)(si->block_size / 4);
    uint64_t direct_cnt = 12;
    uint64_t ind1_cnt   = per_block;
    uint64_t ind2_cnt   = (uint64_t)per_block * per_block;

    if (logical < direct_cnt) {
        *out_blk = di->direct[logical];
        return *out_blk ? 0 : -1;
    }
    logical -= direct_cnt;

    if (logical < ind1_cnt) {
        if (di->indirect1 == 0) return -1;
        uint8_t buf[OBFS_BLOCK_SIZE];
        if (read_block(si, di->indirect1, buf) != 0) return -1;
        uint32_t *arr = (uint32_t *)buf;
        *out_blk = arr[logical];
        return *out_blk ? 0 : -1;
    }
    logical -= ind1_cnt;

    if (logical < ind2_cnt) {
        if (di->indirect2 == 0) return -1;
        uint64_t idx1 = logical / per_block;
        uint64_t idx2 = logical % per_block;
        uint8_t buf1[OBFS_BLOCK_SIZE];
        if (read_block(si, di->indirect2, buf1) != 0) return -1;
        uint32_t *arr1 = (uint32_t *)buf1;
        if (arr1[idx1] == 0) return -1;
        uint8_t buf2[OBFS_BLOCK_SIZE];
        if (read_block(si, arr1[idx1], buf2) != 0) return -1;
        uint32_t *arr2 = (uint32_t *)buf2;
        *out_blk = arr2[idx2];
        return *out_blk ? 0 : -1;
    }
    return -1;
}

/* 从 inode 读取 [offset, offset+count) 到 buf。
 * 越界（offset+count > size）自动截断。返回实际读取字节数。 */
static int64_t obfs_read_range(struct obfs_sb_info *si,
                               struct obfs_inode *di,
                               uint64_t offset, void *buf, uint64_t count)
{
    if (offset >= di->size) return 0;
    uint64_t avail = di->size - offset;
    if (count > avail) count = avail;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t done = 0;

    while (done < count) {
        uint64_t abs_off = offset + done;
        uint64_t logical = abs_off / si->block_size;
        uint64_t in_blk  = abs_off % si->block_size;
        uint64_t this    = si->block_size - in_blk;
        if (this > (count - done)) this = count - done;

        uint32_t phys = 0;
        int have = (map_block(si, di, logical, &phys) == 0);

        if (!have) {
            for (uint64_t i = 0; i < this; ++i) dst[done + i] = 0;
        } else {
            uint8_t blkbuf[OBFS_BLOCK_SIZE];
            if (read_block(si, phys, blkbuf) != 0) {
                return (done > 0) ? (int64_t)done : OB_EIO;
            }
            for (uint64_t i = 0; i < this; ++i)
                dst[done + i] = blkbuf[in_blk + i];
        }
        done += this;
    }
    return (int64_t)done;
}

/* ---------- ops 实现 ---------- */

static int obfs_lookup(struct vfs_inode *dir, const char *name,
                       struct vfs_inode **out)
{
    *out = 0;
    if (!dir || !name) return OB_EINVAL;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return OB_ENOENT;

    struct obfs_sb_info *si =
        (struct obfs_sb_info *)dir->sb->fs_data;
    struct obfs_inode_wrap *dw = (struct obfs_inode_wrap *)dir->fs_data;
    if (!dw) return OB_EIO;

    /* 遍历目录数据块中的 dirent */
    uint64_t bytes_left = dw->disk.size;
    uint64_t logical = 0;
    uint8_t blk[OBFS_BLOCK_SIZE];

    while (bytes_left > 0) {
        uint32_t phys = 0;
        if (map_block(si, &dw->disk, logical, &phys) != 0) {
            ++logical;
            if (bytes_left > si->block_size) bytes_left -= si->block_size;
            else bytes_left = 0;
            continue;
        }
        if (read_block(si, phys, blk) != 0) return OB_EIO;

        uint64_t in_blk_max = si->block_size;
        if (in_blk_max > bytes_left) in_blk_max = bytes_left;

        uint64_t off = 0;
        while (off + sizeof(struct obfs_dirent) <= in_blk_max) {
            struct obfs_dirent *de = (struct obfs_dirent *)(blk + off);
            if (de->ino == 0) { off += sizeof(struct obfs_dirent); continue; }
            if (de->name_len == 0 || de->name_len >= sizeof(de->name))
                { off += sizeof(struct obfs_dirent); continue; }

            /* 比较名称 */
            int eq = 1;
            for (uint16_t i = 0; i < de->name_len; ++i) {
                if (name[i] == '\0' || name[i] != de->name[i]) {
                    eq = 0; break;
                }
            }
            if (eq && name[de->name_len] != '\0') eq = 0;

            if (eq) {
                return obfs_get_inode(dir->sb, de->ino, out);
            }
            off += sizeof(struct obfs_dirent);
        }

        ++logical;
        if (bytes_left > si->block_size) bytes_left -= si->block_size;
        else bytes_left = 0;
    }
    return OB_ENOENT;
}

static int obfs_readdir(struct vfs_inode *dir, uint64_t index,
                        struct vfs_dirent *out)
{
    if (!dir || !out) return OB_EINVAL;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return OB_ENOENT;

    struct obfs_sb_info *si = (struct obfs_sb_info *)dir->sb->fs_data;
    struct obfs_inode_wrap *dw = (struct obfs_inode_wrap *)dir->fs_data;
    if (!dw) return OB_EIO;

    uint64_t seen = 0;
    uint64_t bytes_left = dw->disk.size;
    uint64_t logical = 0;
    uint8_t blk[OBFS_BLOCK_SIZE];

    while (bytes_left > 0) {
        uint32_t phys = 0;
        if (map_block(si, &dw->disk, logical, &phys) != 0) {
            ++logical;
            if (bytes_left > si->block_size) bytes_left -= si->block_size;
            else bytes_left = 0;
            continue;
        }
        if (read_block(si, phys, blk) != 0) return OB_EIO;

        uint64_t in_blk_max = si->block_size;
        if (in_blk_max > bytes_left) in_blk_max = bytes_left;

        uint64_t off = 0;
        while (off + sizeof(struct obfs_dirent) <= in_blk_max) {
            struct obfs_dirent *de = (struct obfs_dirent *)(blk + off);
            off += sizeof(struct obfs_dirent);
            if (de->ino == 0) continue;
            if (de->name_len == 0 || de->name_len >= sizeof(de->name))
                continue;
            if (seen == index) {
                out->ino = de->ino;
                out->type = de->type;
                out->name_len = de->name_len;
                for (uint16_t i = 0; i < de->name_len; ++i)
                    out->name[i] = de->name[i];
                out->name[de->name_len] = '\0';
                return 0;
            }
            ++seen;
        }

        ++logical;
        if (bytes_left > si->block_size) bytes_left -= si->block_size;
        else bytes_left = 0;
    }
    return 1;   /* 越界 */
}

static int64_t obfs_file_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !f->f_inode) return OB_EINVAL;
    struct obfs_sb_info *si = (struct obfs_sb_info *)f->f_inode->sb->fs_data;
    struct obfs_inode_wrap *iw = (struct obfs_inode_wrap *)f->f_inode->fs_data;
    if (!iw) return OB_EIO;

    int64_t n = obfs_read_range(si, &iw->disk, f->f_pos, buf, count);
    if (n > 0) f->f_pos += (uint64_t)n;
    return n;
}

/* OBFS 只读：所有写操作返回 -EROFS */
static int64_t obfs_file_write(struct vfs_file *f, const void *buf,
                               uint64_t count)
{
    (void)f; (void)buf; (void)count;
    return OB_EROFS;
}

static int obfs_create(struct vfs_inode *dir, const char *name,
                       uint16_t mode, struct vfs_inode **out)
{
    (void)dir; (void)name; (void)mode; (void)out;
    return OB_EROFS;
}

static int obfs_mkdir(struct vfs_inode *dir, const char *name,
                      uint16_t mode, struct vfs_inode **out)
{
    (void)dir; (void)name; (void)mode; (void)out;
    return OB_EROFS;
}

static int obfs_unlink(struct vfs_inode *dir, const char *name)
{
    (void)dir; (void)name;
    return OB_EROFS;
}

static int obfs_rmdir(struct vfs_inode *dir, const char *name)
{
    (void)dir; (void)name;
    return OB_EROFS;
}

/* ---------- 挂载 / 卸载 ---------- */

static const struct vfs_operations obfs_ops = {
    .lookup       = obfs_lookup,
    .create       = obfs_create,
    .mkdir        = obfs_mkdir,
    .unlink       = obfs_unlink,
    .rmdir        = obfs_rmdir,
    .read         = obfs_file_read,
    .write        = obfs_file_write,
    .readdir      = obfs_readdir,
    .evict_inode  = obfs_evict_inode,
    .destroy_sb   = obfs_umount,
};

struct vfs_superblock *obfs_mount(const void *image, uint64_t size)
{
    if (!image) return 0;
    if (size < OBFS_BLOCK_SIZE) return 0;

    const struct obfs_superblock *disk =
        (const struct obfs_superblock *)image;

    if (disk->magic != OBFS_MAGIC) {
        serial_printf("[OBFS] mount: bad magic 0x%x\n",
                      (unsigned)disk->magic);
        return 0;
    }
    if (disk->block_size != OBFS_BLOCK_SIZE) {
        serial_printf("[OBFS] mount: bad block size %u\n",
                      (unsigned)disk->block_size);
        return 0;
    }
    if (disk->total_blocks == 0 ||
        disk->total_blocks * OBFS_BLOCK_SIZE > size) {
        serial_printf("[OBFS] mount: total_blocks out of image\n");
        return 0;
    }
    if (disk->root_inode >= disk->inode_count) {
        serial_printf("[OBFS] mount: root_inode out of range\n");
        return 0;
    }

    struct obfs_sb_info *si =
        (struct obfs_sb_info *)kzalloc(sizeof(*si));
    if (!si) return 0;
    si->image             = (const uint8_t *)image;
    si->image_size        = size;
    si->total_blocks      = disk->total_blocks;
    si->block_size        = disk->block_size;
    si->inode_count       = disk->inode_count;
    si->inode_table_start = disk->inode_table_start;
    si->data_block_start  = disk->data_block_start;

    /* 额外边界检查：inode 表不能超出镜像 */
    if (si->inode_table_start * si->block_size
            + si->inode_count * OBFS_INODE_SIZE > size) {
        serial_printf("[OBFS] mount: inode table out of image\n");
        kfree(si);
        return 0;
    }

    struct vfs_superblock *sb =
        (struct vfs_superblock *)kzalloc(sizeof(*sb));
    if (!sb) { kfree(si); return 0; }
    sb->magic      = OBFS_MAGIC;
    sb->block_size = (uint32_t)disk->block_size;
    sb->ops        = &obfs_ops;
    sb->fs_data    = si;

    /* 解析根 inode */
    struct vfs_inode *root = 0;
    int rc = obfs_get_inode(sb, disk->root_inode, &root);
    if (rc != 0) {
        serial_printf("[OBFS] mount: cannot load root inode rc=%d\n", rc);
        kfree(si); kfree(sb);
        return 0;
    }
    if ((root->mode & VFS_S_IFMT) != VFS_S_IFDIR) {
        serial_printf("[OBFS] mount: root not a directory\n");
        kfree(root->fs_data); kfree(root);
        kfree(si); kfree(sb);
        return 0;
    }
    sb->root = root;

    serial_printf("[OBFS] mounted: blocks=%llu inodes=%llu root=%llu\n",
                  (unsigned long long)si->total_blocks,
                  (unsigned long long)si->inode_count,
                  (unsigned long long)disk->root_inode);
    return sb;
}

void obfs_umount(struct vfs_superblock *sb)
{
    if (!sb) return;
    /* 释放根 inode */
    if (sb->root) {
        if (sb->ops && sb->ops->evict_inode)
            sb->ops->evict_inode(sb->root);
        sb->root = 0;
    }
    if (sb->fs_data) {
        kfree(sb->fs_data);
        sb->fs_data = 0;
    }
    kfree(sb);
}

/* ============================================================
 * 第 14 步新增：ITA 公钥访问
 * ============================================================ */

int obfs_get_ita_public_key(struct vfs_superblock *sb, uint8_t out[32])
{
    if (!sb || !out) return OB_EINVAL;
    struct obfs_sb_info *si = (struct obfs_sb_info *)sb->fs_data;
    if (!si || !si->image) return OB_EIO;
    if (si->image_size < sizeof(struct obfs_superblock)) return OB_EIO;

    const struct obfs_superblock *disk =
        (const struct obfs_superblock *)si->image;
    for (int i = 0; i < 32; ++i) out[i] = disk->ita_public_key[i];
    return 0;
}

int obfs_set_ita_public_key(struct vfs_superblock *sb,
                            const uint8_t pub[32])
{
    /* OBFS 是只读文件系统；镜像为 const，无法写入。
     * 第 14 步 root 是 tmpfs（无此字段），走临时密钥回退路径；
     * 真正的持久化写入需要 OBFS 可写挂载（步骤 20+）。 */
    (void)sb; (void)pub;
    return OB_EROFS;
}
/*===OmniBridgeOs/kernel/arch/x64/obfs.c 结束===*/