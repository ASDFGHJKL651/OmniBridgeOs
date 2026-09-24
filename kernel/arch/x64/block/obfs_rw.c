/*===OmniBridgeOs/kernel/arch/x64/block/obfs_rw.c===*/
/*
 * 可写 OBFS 实现（第 18B 步 P0，第 18C 步栈溢出修复）。
 *
 * ★★★ 第 18C 步核心修复 ★★★
 *   原实现在 rw_log_inode / dir_add_entry / dir_remove_entry /
 *   rw_create_or_mkdir / rw_remove_entry / resolve_write /
 *   rw_write_range / obfs_rw_symlink 中使用 uint8_t tmp[PAGE_SIZE]
 *   之类的 4KB 局部变量，深调用链（vfs_mkdir → rw_mkdir →
 *   rw_create_or_mkdir → rw_log_inode → dir_add_entry）中累积超过
 *   20KB，触发内核栈溢出，返回地址被破坏为 0xb0001，CPU 执行
 *   非法字节触发 #UD（[PANIC] Exception 6）。
 *
 *   修复方式：所有元数据写操作的 4KB 栈缓冲改为静态缓冲。由于
 *   OBFS-RW 的所有元数据写操作由 journal 单事务保护（j->active
 *   标志），不存在并发重入；缓冲按"函数级别顺序使用"，不会嵌套
 *   冲突。这既消除了栈溢出，又保留了原有的语义。
 */
#include "obfs_rw.h"
#include "page_cache.h"
#include "journal.h"
#include "quota.h"
#include "symlink.h"
#include "crypto.h"
#include "kmalloc.h"
#include "serial.h"
#include "spinlock.h"
#include "pmm.h"

#define SECTORS_PER_BLOCK (PAGE_SIZE / BLOCK_SECTOR_SIZE)
#define NDIRECT   12u
#define NINDIR    (PAGE_SIZE / 4u)

struct obfs_rw_info {
    struct block_device *dev;
    uint64_t             start_block;
    uint64_t             total_blocks;
    uint64_t             inode_count;
    uint64_t             block_bitmap_start;
    uint64_t             inode_bitmap_start;
    uint64_t             inode_table_start;
    uint64_t             data_block_start;
    uint64_t             root_inode;
    uint32_t             journal_start;
    uint32_t             journal_size;
    int                  writable;
    struct journal       jnl;
    spinlock_t           lock;
};

/* ============================================================
 * ★★★ 第 18C 步修复：静态页缓冲池 ★★★
 *
 * 每个缓冲 4KB，位于 .bss。所有 OBFS-RW 元数据写函数内的
 * `uint8_t tmp[PAGE_SIZE]` 全部替换为对这些缓冲的引用。
 *
 * 用途分配（按函数级别，不会嵌套冲突）：
 *   pb0  : rw_log_inode
 *   pb1  : dir_add_entry / dir_remove_entry 主缓冲
 *   pb2  : dir_add_entry 块位图
 *   pb3  : dir_add_entry 数据块
 *   pb4  : rw_create_or_mkdir inode 位图
 *   pb5  : rw_remove_entry 块位图
 *   pb6  : rw_remove_entry inode 位图
 *   pb7  : resolve_write 位图（含 ALLOC_DATA_RET 宏）
 *   pb8  : resolve_write 间接块初始化
 *   pb9  : rw_write_range 数据块
 *   pb10 : obfs_rw_symlink 数据块
 *   pb11 : obfs_rw_symlink 块位图
 *   pb12 : obfs_rw_symlink inode 位图
 * ============================================================ */
static uint8_t g_rw_pb0 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb1 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb2 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb3 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb4 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb5 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb6 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb7 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb8 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb9 [PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb10[PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb11[PAGE_SIZE] __attribute__((aligned(16)));
static uint8_t g_rw_pb12[PAGE_SIZE] __attribute__((aligned(16)));

/* ---------- 字节工具 ---------- */
static void byte_copy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; ++i) d[i] = s[i];
}

static void byte_zero(void *p, uint64_t n)
{
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; ++i) b[i] = 0;
}

/* ---------- 页缓存读写 ---------- */
static int rw_read_block(struct obfs_rw_info *ri, uint64_t block_no,
                         uint8_t **out)
{
    return pcache_get(ri->dev, ri->start_block + block_no, out, 0);
}

static int rw_write_block(struct obfs_rw_info *ri, uint64_t block_no,
                          uint8_t **out)
{
    return pcache_get(ri->dev, ri->start_block + block_no, out, 1);
}

/* ---------- 位图 ---------- */
static inline int bm_test(const uint8_t *bm, uint64_t i)
{ return (bm[i >> 3] >> (i & 7)) & 1; }
static inline void bm_set(uint8_t *bm, uint64_t i)
{ bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bm_clear(uint8_t *bm, uint64_t i)
{ bm[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

/* ---------- inode 读写 ---------- */
static uint64_t rw_inode_offset(struct obfs_rw_info *ri, uint64_t ino)
{
    return ri->inode_table_start * PAGE_SIZE + ino * OBFS_RW_INODE_SIZE;
}

static int rw_read_inode(struct obfs_rw_info *ri, uint64_t ino,
                         struct obfs_inode_rw *out)
{
    if (ino >= ri->inode_count) return -5;
    uint64_t off = rw_inode_offset(ri, ino);
    uint64_t block_no = off / PAGE_SIZE;
    uint32_t in_block = (uint32_t)(off % PAGE_SIZE);

    uint8_t *blk = 0;
    int rc = rw_read_block(ri, block_no, &blk);
    if (rc != 0) return rc;

    byte_copy(out, blk + in_block, OBFS_RW_INODE_SIZE);
    return 0;
}

/*
 * ★ 关键修复：rw_log_inode 使用 g_rw_pb0 替代 4KB 栈变量。
 *   流程：for_write=1 读 pcache → 拷贝到 pb0 → 修改 inode 部分
 *   → journal_add 记录 pb0 → 把修改写回 pcache。
 */
static int rw_log_inode(struct journal_txn *txn, struct obfs_rw_info *ri,
                        uint64_t ino, const struct obfs_inode_rw *in)
{
    if (ino >= ri->inode_count) return -5;

    uint64_t off = rw_inode_offset(ri, ino);
    uint64_t block_no = ri->start_block + off / PAGE_SIZE;

    uint8_t *blk = 0;
    int rc = pcache_get(ri->dev, block_no, &blk, 1);
    if (rc != 0) return rc;

    uint8_t *tmp = g_rw_pb0;                    /* ★ 静态缓冲 */
    byte_copy(tmp, blk, PAGE_SIZE);

    uint32_t in_block = (uint32_t)(off % PAGE_SIZE);
    byte_copy(tmp + in_block, in, OBFS_RW_INODE_SIZE);

    rc = journal_add(txn, block_no, tmp);
    if (rc != 0) return rc;

    byte_copy(blk, tmp, PAGE_SIZE);
    pcache_mark_dirty(ri->dev, block_no);
    return 0;
}

/* ---------- 数据块分配 ---------- */
static int alloc_data_block(struct obfs_rw_info *ri, uint8_t *bitmap_buf,
                            uint64_t *out_blk)
{
    for (uint64_t i = ri->data_block_start;
         i < (uint64_t)PAGE_SIZE * 8; ++i) {
        if (!bm_test(bitmap_buf, i)) {
            bm_set(bitmap_buf, i);
            *out_blk = i - ri->data_block_start;
            return 0;
        }
    }
    return -1;
}

/* ---------- 块映射（读） ---------- */
static int resolve_read(struct obfs_rw_info *ri, struct obfs_inode_rw *ino,
                        uint64_t logical, uint64_t *out_phys)
{
    *out_phys = 0;
    if (logical < NDIRECT) {
        if (ino->base.direct[logical] == 0) return -1;
        *out_phys = ino->base.direct[logical];
        return 0;
    }
    logical -= NDIRECT;

    if (logical < NINDIR) {
        if (ino->base.indirect1 == 0) return -1;
        uint8_t *b = 0;
        if (rw_read_block(ri, ino->base.indirect1, &b) != 0) return -5;
        uint32_t *tbl = (uint32_t *)b;
        if (tbl[logical] == 0) return -1;
        *out_phys = tbl[logical];
        return 0;
    }
    logical -= NINDIR;

    if (logical < (uint64_t)NINDIR * NINDIR) {
        if (ino->base.indirect2 == 0) return -1;
        uint32_t l1 = (uint32_t)(logical / NINDIR);
        uint32_t l2 = (uint32_t)(logical % NINDIR);
        uint8_t *b1 = 0;
        if (rw_read_block(ri, ino->base.indirect2, &b1) != 0) return -5;
        uint32_t *t1 = (uint32_t *)b1;
        if (t1[l1] == 0) return -1;
        uint8_t *b2 = 0;
        if (rw_read_block(ri, t1[l1], &b2) != 0) return -5;
        uint32_t *t2 = (uint32_t *)b2;
        if (t2[l2] == 0) return -1;
        *out_phys = t2[l2];
        return 0;
    }
    return -1;
}

/* ---------- 块映射（写） ---------- */
/* ★ ALLOC_DATA_RET 改用 g_rw_pb7（静态缓冲） */
#define ALLOC_DATA_RET(phys_out) do {                       \
    uint64_t __nb = 0;                                      \
    if (alloc_data_block(ri, bm, &__nb) != 0) {             \
        return -28;                                         \
    }                                                       \
    uint8_t *__tmp_bm = g_rw_pb7;                           \
    byte_copy(__tmp_bm, bm, PAGE_SIZE);                     \
    journal_add(txn, ri->start_block +                      \
                ri->block_bitmap_start, __tmp_bm);          \
    *(phys_out) = ri->data_block_start + __nb;              \
} while (0)

static int resolve_write(struct obfs_rw_info *ri,
                         struct journal_txn *txn,
                         struct obfs_inode_rw *ino,
                         uint64_t logical, uint64_t *out_phys)
{
    uint8_t *bm = 0;
    if (pcache_get(ri->dev,
                   ri->start_block + ri->block_bitmap_start, &bm, 1) != 0)
        return -5;

    if (logical < NDIRECT) {
        if (ino->base.direct[logical] == 0) {
            uint64_t newblk;
            ALLOC_DATA_RET(&newblk);
            ino->base.direct[logical] = (uint32_t)newblk;
            *out_phys = newblk;
        } else {
            *out_phys = ino->base.direct[logical];
        }
        return 0;
    }
    logical -= NDIRECT;

    if (logical < NINDIR) {
        if (ino->base.indirect1 == 0) {
            uint64_t newblk;
            ALLOC_DATA_RET(&newblk);
            ino->base.indirect1 = (uint32_t)newblk;

            uint8_t *ib = 0;
            if (rw_write_block(ri, (uint32_t)newblk, &ib) != 0) return -5;
            byte_zero(ib, PAGE_SIZE);
            uint8_t *tmp = g_rw_pb8;             /* ★ 静态缓冲 */
            byte_zero(tmp, PAGE_SIZE);
            journal_add(txn, ri->start_block + (uint32_t)newblk, tmp);
        }
        uint8_t *b = 0;
        if (rw_write_block(ri, ino->base.indirect1, &b) != 0) return -5;
        uint32_t *tbl = (uint32_t *)b;
        if (tbl[logical] == 0) {
            uint64_t newblk;
            ALLOC_DATA_RET(&newblk);
            tbl[logical] = (uint32_t)newblk;

            uint8_t *tmp = g_rw_pb8;             /* ★ 复用 */
            byte_copy(tmp, b, PAGE_SIZE);
            journal_add(txn, ri->start_block + ino->base.indirect1, tmp);
        }
        *out_phys = tbl[logical];
        return 0;
    }
    logical -= NINDIR;

    if (logical < (uint64_t)NINDIR * NINDIR) {
        uint32_t l1 = (uint32_t)(logical / NINDIR);
        uint32_t l2 = (uint32_t)(logical % NINDIR);

        if (ino->base.indirect2 == 0) {
            uint64_t newblk;
            ALLOC_DATA_RET(&newblk);
            ino->base.indirect2 = (uint32_t)newblk;

            uint8_t *ib = 0;
            if (rw_write_block(ri, (uint32_t)newblk, &ib) != 0) return -5;
            byte_zero(ib, PAGE_SIZE);
            uint8_t *tmp = g_rw_pb8;             /* ★ 复用 */
            byte_zero(tmp, PAGE_SIZE);
            journal_add(txn, ri->start_block + (uint32_t)newblk, tmp);
        }
        uint8_t *b1 = 0;
        if (rw_write_block(ri, ino->base.indirect2, &b1) != 0) return -5;
        uint32_t *t1 = (uint32_t *)b1;

        if (t1[l1] == 0) {
            uint64_t newblk;
            ALLOC_DATA_RET(&newblk);
            t1[l1] = (uint32_t)newblk;

            uint8_t *b2 = 0;
            if (rw_write_block(ri, (uint32_t)newblk, &b2) != 0) return -5;
            byte_zero(b2, PAGE_SIZE);
            uint8_t *tmp = g_rw_pb8;             /* ★ 复用 */
            byte_zero(tmp, PAGE_SIZE);
            journal_add(txn, ri->start_block + (uint32_t)newblk, tmp);

            uint8_t *tmp1 = g_rw_pb8;            /* ★ 复用（顺序） */
            byte_copy(tmp1, b1, PAGE_SIZE);
            journal_add(txn, ri->start_block + ino->base.indirect2, tmp1);
        }

        uint8_t *b2 = 0;
        if (rw_write_block(ri, t1[l1], &b2) != 0) return -5;
        uint32_t *t2 = (uint32_t *)b2;
        if (t2[l2] == 0) {
            uint64_t newblk;
            ALLOC_DATA_RET(&newblk);
            t2[l2] = (uint32_t)newblk;

            uint8_t *tmp2 = g_rw_pb8;            /* ★ 复用 */
            byte_copy(tmp2, b2, PAGE_SIZE);
            journal_add(txn, ri->start_block + t1[l1], tmp2);
        }
        *out_phys = t2[l2];
        return 0;
    }

    return -28;
}

#undef ALLOC_DATA_RET

/* ---------- VFS ops 前向声明 ---------- */
static int rw_lookup(struct vfs_inode *dir, const char *name,
                     struct vfs_inode **out);
static int rw_create(struct vfs_inode *dir, const char *name,
                     uint16_t mode, struct vfs_inode **out);
static int rw_mkdir(struct vfs_inode *dir, const char *name,
                    uint16_t mode, struct vfs_inode **out);
static int rw_unlink(struct vfs_inode *dir, const char *name);
static int rw_rmdir(struct vfs_inode *dir, const char *name);
static int64_t rw_read(struct vfs_file *f, void *buf, uint64_t count);
static int64_t rw_write(struct vfs_file *f, const void *buf, uint64_t count);
static int rw_readdir(struct vfs_inode *dir, uint64_t index,
                      struct vfs_dirent *out);
static void rw_evict_inode(struct vfs_inode *inode);
static void rw_destroy_sb(struct vfs_superblock *sb);

static const struct vfs_operations g_obfs_rw_ops = {
    .lookup      = rw_lookup,
    .create      = rw_create,
    .mkdir       = rw_mkdir,
    .unlink      = rw_unlink,
    .rmdir       = rw_rmdir,
    .read        = rw_read,
    .write       = rw_write,
    .readdir     = rw_readdir,
    .evict_inode = rw_evict_inode,
    .destroy_sb  = rw_destroy_sb,
};

/* ---------- 目录项遍历 ---------- */
static int rw_dir_walk(struct obfs_rw_info *ri, struct obfs_inode_rw *dir,
                       int (*cb)(struct obfs_dirent *de, void *arg),
                       void *arg)
{
    for (uint32_t i = 0; i < NDIRECT; ++i) {
        uint32_t blk = dir->base.direct[i];
        if (blk == 0) continue;
        uint8_t *b = 0;
        if (rw_read_block(ri, blk, &b) != 0) continue;
        for (uint32_t off = 0;
             off + sizeof(struct obfs_dirent) <= PAGE_SIZE;
             off += sizeof(struct obfs_dirent)) {
            struct obfs_dirent *de = (struct obfs_dirent *)(b + off);
            if (de->ino == 0) continue;
            int stop = cb(de, arg);
            if (stop) return stop;
        }
    }
    return 0;
}

struct lookup_ctx {
    const char *name;
    uint64_t    found_ino;
    uint16_t    found_type;
    int         found;
};

static int lookup_cb(struct obfs_dirent *de, void *arg)
{
    struct lookup_ctx *c = (struct lookup_ctx *)arg;
    if (de->name_len == 0) return 0;
    for (uint16_t i = 0; i < de->name_len; ++i) {
        if (c->name[i] == '\0' || c->name[i] != de->name[i]) return 0;
    }
    if (c->name[de->name_len] != '\0') return 0;
    c->found_ino  = de->ino;
    c->found_type = de->type;
    c->found      = 1;
    return 1;
}

static int rw_get_inode_vfs(struct vfs_superblock *sb, uint64_t ino,
                            struct vfs_inode **out)
{
    struct obfs_rw_info *ri = (struct obfs_rw_info *)sb->fs_data;
    struct obfs_inode_rw rw;
    int rc = rw_read_inode(ri, ino, &rw);
    if (rc != 0) return rc;

    struct vfs_inode *vi = (struct vfs_inode *)kzalloc(sizeof(*vi));
    if (!vi) return -12;
    struct obfs_rw_inode_wrap *wr =
        (struct obfs_rw_inode_wrap *)kzalloc(sizeof(*wr));
    if (!wr) { kfree(vi); return -12; }
    byte_copy(&wr->disk, &rw, sizeof(rw));

    vi->ino   = ino;
    vi->mode  = rw.base.mode;
    vi->size  = rw.base.size;
    vi->atime = rw.base.atime;
    vi->mtime = rw.base.mtime;
    vi->ctime = rw.base.ctime;
    vi->sb    = sb;
    vi->ops   = sb->ops;
    vi->fs_data = wr;

    *out = vi;
    return 0;
}

struct obfs_inode_rw *obfs_rw_disk_inode(struct vfs_inode *vi)
{
    if (!vi || !vi->fs_data) return 0;
    struct obfs_rw_inode_wrap *wr = (struct obfs_rw_inode_wrap *)vi->fs_data;
    return &wr->disk;
}

static int rw_lookup(struct vfs_inode *dir, const char *name,
                     struct vfs_inode **out)
{
    *out = 0;
    if (!dir || !name) return -22;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return -20;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)dir->sb->fs_data;
    struct obfs_rw_inode_wrap *dw = (struct obfs_rw_inode_wrap *)dir->fs_data;
    if (!dw) return -5;

    struct lookup_ctx c = { name, 0, 0, 0 };
    rw_dir_walk(ri, &dw->disk, lookup_cb, &c);
    if (!c.found) return -2;
    return rw_get_inode_vfs(dir->sb, c.found_ino, out);
}

/* ---------- 目录项添加/删除 ---------- */
/*
 * ★ 关键修复：dir_add_entry 使用 g_rw_pb1/pb2/pb3 替代 3 个 4KB
 *   栈变量。三个缓冲按"主缓冲 → 块位图 → 数据块"的顺序使用，
 *   不会同时活跃。
 */
static int dir_add_entry(struct obfs_rw_info *ri, struct journal_txn *txn,
                         struct obfs_inode_rw *dir, uint64_t ino,
                         uint16_t type, const char *name)
{
    uint16_t nl = 0;
    while (name[nl]) nl++;
    if (nl == 0 || nl >= sizeof(((struct obfs_dirent *)0)->name))
        return -36;

    for (uint32_t i = 0; i < NDIRECT; ++i) {
        if (dir->base.direct[i] == 0) continue;
        uint32_t blk = dir->base.direct[i];
        uint8_t *b = 0;
        if (pcache_get(ri->dev, ri->start_block + blk, &b, 1) != 0) continue;

        for (uint32_t off = 0;
             off + sizeof(struct obfs_dirent) <= PAGE_SIZE;
             off += sizeof(struct obfs_dirent)) {
            struct obfs_dirent *de = (struct obfs_dirent *)(b + off);
            if (de->ino != 0) continue;

            uint8_t *tmp = g_rw_pb1;             /* ★ 静态缓冲 */
            byte_copy(tmp, b, PAGE_SIZE);
            struct obfs_dirent *nde = (struct obfs_dirent *)(tmp + off);
            nde->ino = ino;
            nde->type = type;
            nde->name_len = nl;
            for (uint16_t k = 0; k < nl; ++k) nde->name[k] = name[k];
            nde->name[nl] = '\0';

            int rc = journal_add(txn, ri->start_block + blk, tmp);
            if (rc != 0) return rc;

            byte_copy(b, tmp, PAGE_SIZE);
            pcache_mark_dirty(ri->dev, ri->start_block + blk);
            return 0;
        }
    }

    uint8_t *bm = 0;
    if (pcache_get(ri->dev,
                   ri->start_block + ri->block_bitmap_start, &bm, 1) != 0)
        return -5;

    uint64_t newblk = 0;
    if (alloc_data_block(ri, bm, &newblk) != 0) return -28;

    uint8_t *tmp_bm = g_rw_pb2;                  /* ★ 静态缓冲 */
    byte_copy(tmp_bm, bm, PAGE_SIZE);
    journal_add(txn, ri->start_block + ri->block_bitmap_start, tmp_bm);

    uint8_t *tmp_data = g_rw_pb3;                /* ★ 静态缓冲 */
    byte_zero(tmp_data, PAGE_SIZE);
    struct obfs_dirent *de = (struct obfs_dirent *)tmp_data;
    de->ino      = ino;
    de->type     = type;
    de->name_len = nl;
    for (uint16_t k = 0; k < nl; ++k) de->name[k] = name[k];
    de->name[nl] = '\0';

    uint64_t abs_blk = ri->data_block_start + newblk;
    journal_add(txn, ri->start_block + abs_blk, tmp_data);

    int slot = -1;
    for (uint32_t i = 0; i < NDIRECT; ++i)
        if (dir->base.direct[i] == 0) { slot = (int)i; break; }
    if (slot < 0) return -28;
    dir->base.direct[slot] = (uint32_t)abs_blk;
    dir->base.size += PAGE_SIZE;

    byte_copy(bm, tmp_bm, PAGE_SIZE);
    pcache_mark_dirty(ri->dev, ri->start_block + ri->block_bitmap_start);
    pcache_invalidate(ri->dev, ri->start_block + abs_blk);
    return 0;
}

/* ★ dir_remove_entry 使用 g_rw_pb1（与 dir_add_entry 复用） */
static int dir_remove_entry(struct obfs_rw_info *ri, struct journal_txn *txn,
                            struct obfs_inode_rw *dir, uint64_t ino)
{
    for (uint32_t i = 0; i < NDIRECT; ++i) {
        if (dir->base.direct[i] == 0) continue;
        uint32_t blk = dir->base.direct[i];
        uint8_t *b = 0;
        if (pcache_get(ri->dev, ri->start_block + blk, &b, 1) != 0) continue;

        for (uint32_t off = 0;
             off + sizeof(struct obfs_dirent) <= PAGE_SIZE;
             off += sizeof(struct obfs_dirent)) {
            struct obfs_dirent *de = (struct obfs_dirent *)(b + off);
            if (de->ino != ino) continue;

            uint8_t *tmp = g_rw_pb1;             /* ★ 静态缓冲 */
            byte_copy(tmp, b, PAGE_SIZE);
            struct obfs_dirent *nde = (struct obfs_dirent *)(tmp + off);
            nde->ino = 0;
            nde->type = 0;
            nde->name_len = 0;
            nde->name[0] = '\0';

            journal_add(txn, ri->start_block + blk, tmp);
            byte_copy(b, tmp, PAGE_SIZE);
            pcache_mark_dirty(ri->dev, ri->start_block + blk);
            return 0;
        }
    }
    return -2;
}

/* ---------- inode 位图分配 ---------- */
static int alloc_inode(struct obfs_rw_info *ri, uint8_t *ibm, uint64_t *out)
{
    for (uint64_t i = 0; i < ri->inode_count; ++i) {
        if (!bm_test(ibm, i)) { bm_set(ibm, i); *out = i; return 0; }
    }
    return -1;
}

/* ---------- create / mkdir ---------- */
/*
 * ★ 关键修复：rw_create_or_mkdir 使用 g_rw_pb4 替代 4KB 栈变量。
 */
static int rw_create_or_mkdir(struct vfs_inode *dir, const char *name,
                              uint16_t mode, struct vfs_inode **out,
                              int is_dir)
{
    if (!dir || !name || !out) return -22;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return -20;
    *out = 0;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)dir->sb->fs_data;
    if (!ri->writable) return -30;

    struct obfs_rw_inode_wrap *dw = (struct obfs_rw_inode_wrap *)dir->fs_data;
    if (!dw) return -5;

    struct lookup_ctx c = { name, 0, 0, 0 };
    rw_dir_walk(ri, &dw->disk, lookup_cb, &c);
    if (c.found) return -17;

    struct journal_txn *txn = journal_begin(&ri->jnl);
    if (!txn) return -11;

    uint8_t *ibm = 0;
    if (pcache_get(ri->dev,
                   ri->start_block + ri->inode_bitmap_start, &ibm, 1) != 0) {
        journal_abort(&ri->jnl, txn);
        return -5;
    }

    uint64_t new_ino = 0;
    if (alloc_inode(ri, ibm, &new_ino) != 0) {
        journal_abort(&ri->jnl, txn);
        return -28;
    }

    struct obfs_inode_rw ni;
    byte_zero(&ni, sizeof(ni));
    ni.base.mode  = (uint16_t)(is_dir ? (VFS_S_IFDIR | (mode & 0x0FFF))
                                      : (VFS_S_IFREG | (mode & 0x0FFF)));
    ni.base.links = 1;
    ni.parent_ino = dir->ino;
    ni.link_count = 1;
    ni.quota_limit = 0;

    int rc = rw_log_inode(txn, ri, new_ino, &ni);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    uint8_t *tmp_ibm = g_rw_pb4;                 /* ★ 静态缓冲 */
    byte_copy(tmp_ibm, ibm, PAGE_SIZE);
    journal_add(txn, ri->start_block + ri->inode_bitmap_start, tmp_ibm);

    rc = dir_add_entry(ri, txn, &dw->disk, new_ino,
                       is_dir ? VFS_FT_DIR : VFS_FT_REG, name);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    rc = rw_log_inode(txn, ri, dir->ino, &dw->disk);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    rc = journal_commit(&ri->jnl, txn);
    if (rc != 0) return rc;

    pcache_invalidate(ri->dev,
        ri->start_block + (ri->inode_table_start +
                          (new_ino * OBFS_RW_INODE_SIZE) / PAGE_SIZE));
    pcache_mark_dirty(ri->dev, ri->start_block + ri->inode_bitmap_start);
    dir->size = dw->disk.base.size;

    return rw_get_inode_vfs(dir->sb, new_ino, out);
}

static int rw_create(struct vfs_inode *dir, const char *name,
                     uint16_t mode, struct vfs_inode **out)
{ return rw_create_or_mkdir(dir, name, mode, out, 0); }

static int rw_mkdir(struct vfs_inode *dir, const char *name,
                    uint16_t mode, struct vfs_inode **out)
{ return rw_create_or_mkdir(dir, name, mode, out, 1); }

/* ---------- 释放 inode 数据块 + indirect ---------- */
static void free_inode_blocks(struct obfs_rw_info *ri,
                              struct journal_txn *txn,
                              struct obfs_inode_rw *ino,
                              uint8_t *bm)
{
    (void)txn;
    for (uint32_t i = 0; i < NDIRECT; ++i) {
        if (ino->base.direct[i]) {
            bm_clear(bm, ino->base.direct[i] - ri->data_block_start);
            ino->base.direct[i] = 0;
        }
    }
    if (ino->base.indirect1) {
        uint8_t *b = 0;
        if (pcache_get(ri->dev, ri->start_block + ino->base.indirect1,
                       &b, 0) == 0) {
            uint32_t *t = (uint32_t *)b;
            for (uint32_t i = 0; i < NINDIR; ++i) {
                if (t[i]) bm_clear(bm, t[i] - ri->data_block_start);
            }
        }
        bm_clear(bm, ino->base.indirect1 - ri->data_block_start);
        ino->base.indirect1 = 0;
    }
    if (ino->base.indirect2) {
        uint8_t *b1 = 0;
        if (pcache_get(ri->dev, ri->start_block + ino->base.indirect2,
                       &b1, 0) == 0) {
            uint32_t *t1 = (uint32_t *)b1;
            for (uint32_t i = 0; i < NINDIR; ++i) {
                if (t1[i] == 0) continue;
                uint8_t *b2 = 0;
                if (pcache_get(ri->dev, ri->start_block + t1[i],
                               &b2, 0) == 0) {
                    uint32_t *t2 = (uint32_t *)b2;
                    for (uint32_t k = 0; k < NINDIR; ++k) {
                        if (t2[k]) bm_clear(bm, t2[k] - ri->data_block_start);
                    }
                }
                bm_clear(bm, t1[i] - ri->data_block_start);
            }
        }
        bm_clear(bm, ino->base.indirect2 - ri->data_block_start);
        ino->base.indirect2 = 0;
    }
}

/*
 * ★ 关键修复：rw_remove_entry 使用 g_rw_pb5/pb6 替代 2 个 4KB 栈变量。
 */
static int rw_remove_entry(struct vfs_inode *dir, const char *name,
                           int is_dir)
{
    if (!dir || !name) return -22;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return -20;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)dir->sb->fs_data;
    if (!ri->writable) return -30;

    struct obfs_rw_inode_wrap *dw = (struct obfs_rw_inode_wrap *)dir->fs_data;

    struct lookup_ctx c = { name, 0, 0, 0 };
    rw_dir_walk(ri, &dw->disk, lookup_cb, &c);
    if (!c.found) return -2;

    struct obfs_inode_rw target;
    int rc = rw_read_inode(ri, c.found_ino, &target);
    if (rc != 0) return rc;

    int target_is_dir = ((target.base.mode & VFS_S_IFMT) == VFS_S_IFDIR);
    if (is_dir && !target_is_dir) return -20;
    if (!is_dir && target_is_dir) return -21;

    if (target_is_dir) {
        int nonempty = 0;
        for (uint32_t i = 0; i < NDIRECT && !nonempty; ++i) {
            if (target.base.direct[i] == 0) continue;
            uint8_t *b = 0;
            if (pcache_get(ri->dev, ri->start_block + target.base.direct[i],
                           &b, 0) != 0) continue;
            for (uint32_t off = 0;
                 off + sizeof(struct obfs_dirent) <= PAGE_SIZE;
                 off += sizeof(struct obfs_dirent)) {
                struct obfs_dirent *de = (struct obfs_dirent *)(b + off);
                if (de->ino != 0) { nonempty = 1; break; }
            }
        }
        if (nonempty) return -39;
    }

    struct journal_txn *txn = journal_begin(&ri->jnl);
    if (!txn) return -11;

    rc = dir_remove_entry(ri, txn, &dw->disk, c.found_ino);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    rc = rw_log_inode(txn, ri, dir->ino, &dw->disk);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    if (target.link_count > 1) {
        target.link_count--;
        rc = rw_log_inode(txn, ri, c.found_ino, &target);
        if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }
    } else {
        uint8_t *bm = 0;
        if (pcache_get(ri->dev,
                       ri->start_block + ri->block_bitmap_start,
                       &bm, 1) == 0) {
            free_inode_blocks(ri, txn, &target, bm);
            uint8_t *tmp_bm = g_rw_pb5;          /* ★ 静态缓冲 */
            byte_copy(tmp_bm, bm, PAGE_SIZE);
            journal_add(txn, ri->start_block + ri->block_bitmap_start, tmp_bm);
            byte_copy(bm, tmp_bm, PAGE_SIZE);
            pcache_mark_dirty(ri->dev,
                              ri->start_block + ri->block_bitmap_start);
        }

        struct obfs_inode_rw empty;
        byte_zero(&empty, sizeof(empty));
        rw_log_inode(txn, ri, c.found_ino, &empty);

        uint8_t *ibm = 0;
        if (pcache_get(ri->dev,
                       ri->start_block + ri->inode_bitmap_start,
                       &ibm, 1) == 0) {
            bm_clear(ibm, c.found_ino);
            uint8_t *tmp_ibm = g_rw_pb6;         /* ★ 静态缓冲 */
            byte_copy(tmp_ibm, ibm, PAGE_SIZE);
            journal_add(txn, ri->start_block + ri->inode_bitmap_start,
                        tmp_ibm);
            byte_copy(ibm, tmp_ibm, PAGE_SIZE);
            pcache_mark_dirty(ri->dev,
                              ri->start_block + ri->inode_bitmap_start);
        }
    }

    rc = journal_commit(&ri->jnl, txn);
    if (rc != 0) return rc;

    pcache_invalidate(ri->dev,
        ri->start_block + ri->inode_table_start +
        (c.found_ino * OBFS_RW_INODE_SIZE) / PAGE_SIZE);

    dir->size = dw->disk.base.size;
    return 0;
}

static int rw_unlink(struct vfs_inode *dir, const char *name)
{ return rw_remove_entry(dir, name, 0); }

static int rw_rmdir(struct vfs_inode *dir, const char *name)
{ return rw_remove_entry(dir, name, 1); }

/* ---------- 硬链接 ---------- */
int obfs_rw_link(struct vfs_inode *dir, const char *name,
                 struct vfs_inode *existing)
{
    if (!dir || !name || !existing) return -22;
    if (existing->sb != dir->sb) return -18;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)dir->sb->fs_data;
    if (!ri->writable) return -30;

    struct obfs_rw_inode_wrap *dw = (struct obfs_rw_inode_wrap *)dir->fs_data;
    struct obfs_rw_inode_wrap *ew =
        (struct obfs_rw_inode_wrap *)existing->fs_data;

    struct lookup_ctx c = { name, 0, 0, 0 };
    rw_dir_walk(ri, &dw->disk, lookup_cb, &c);
    if (c.found) return -17;

    struct journal_txn *txn = journal_begin(&ri->jnl);
    if (!txn) return -11;

    ew->disk.link_count++;
    ew->disk.base.links = (uint16_t)ew->disk.link_count;

    int rc = rw_log_inode(txn, ri, existing->ino, &ew->disk);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    uint16_t ftype = (uint16_t)((existing->mode & VFS_S_IFMT) == VFS_S_IFDIR
                                ? VFS_FT_DIR : VFS_FT_REG);
    rc = dir_add_entry(ri, txn, &dw->disk, existing->ino, ftype, name);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    rc = rw_log_inode(txn, ri, dir->ino, &dw->disk);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    return journal_commit(&ri->jnl, txn);
}

/*
 * ★ 关键修复：obfs_rw_symlink 使用 g_rw_pb10/pb11/pb12 替代
 *   3 个 4KB 栈变量。
 */
int obfs_rw_symlink(struct vfs_inode *dir, const char *name,
                    const char *target)
{
    if (!dir || !name || !target) return -22;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return -20;
    if (symlink_check_target(target) != 0) return -1;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)dir->sb->fs_data;
    if (!ri->writable) return -30;

    struct obfs_rw_inode_wrap *dw = (struct obfs_rw_inode_wrap *)dir->fs_data;

    struct lookup_ctx c = { name, 0, 0, 0 };
    rw_dir_walk(ri, &dw->disk, lookup_cb, &c);
    if (c.found) return -17;

    struct journal_txn *txn = journal_begin(&ri->jnl);
    if (!txn) return -11;

    uint8_t *ibm = 0;
    if (pcache_get(ri->dev,
                   ri->start_block + ri->inode_bitmap_start, &ibm, 1) != 0) {
        journal_abort(&ri->jnl, txn);
        return -5;
    }
    uint64_t new_ino = 0;
    if (alloc_inode(ri, ibm, &new_ino) != 0) {
        journal_abort(&ri->jnl, txn);
        return -28;
    }

    uint32_t tlen = 0;
    while (target[tlen]) tlen++;
    if (tlen >= PAGE_SIZE) { journal_abort(&ri->jnl, txn); return -22; }

    uint8_t *bm = 0;
    if (pcache_get(ri->dev,
                   ri->start_block + ri->block_bitmap_start, &bm, 1) != 0) {
        journal_abort(&ri->jnl, txn);
        return -5;
    }
    uint64_t dblk = 0;
    if (alloc_data_block(ri, bm, &dblk) != 0) {
        journal_abort(&ri->jnl, txn);
        return -28;
    }
    uint64_t abs_dblk = ri->data_block_start + dblk;

    uint8_t *tmp_data = g_rw_pb10;               /* ★ 静态缓冲 */
    byte_zero(tmp_data, PAGE_SIZE);
    for (uint32_t i = 0; i < tlen; ++i) tmp_data[i] = (uint8_t)target[i];
    journal_add(txn, ri->start_block + abs_dblk, tmp_data);

    uint8_t *tmp_bm = g_rw_pb11;                 /* ★ 静态缓冲 */
    byte_copy(tmp_bm, bm, PAGE_SIZE);
    journal_add(txn, ri->start_block + ri->block_bitmap_start, tmp_bm);

    struct obfs_inode_rw ni;
    byte_zero(&ni, sizeof(ni));
    ni.base.mode  = VFS_S_IFLNK | 0777;
    ni.base.links = 1;
    ni.base.size  = tlen;
    ni.base.direct[0] = (uint32_t)abs_dblk;
    ni.parent_ino = dir->ino;
    ni.link_count = 1;

    int rc = rw_log_inode(txn, ri, new_ino, &ni);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    uint8_t *tmp_ibm = g_rw_pb12;                /* ★ 静态缓冲 */
    byte_copy(tmp_ibm, ibm, PAGE_SIZE);
    journal_add(txn, ri->start_block + ri->inode_bitmap_start, tmp_ibm);

    rc = dir_add_entry(ri, txn, &dw->disk, new_ino, VFS_FT_LNK, name);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    rc = rw_log_inode(txn, ri, dir->ino, &dw->disk);
    if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

    rc = journal_commit(&ri->jnl, txn);
    if (rc != 0) return rc;

    byte_copy(bm, tmp_bm, PAGE_SIZE);
    pcache_mark_dirty(ri->dev, ri->start_block + ri->block_bitmap_start);
    byte_copy(ibm, tmp_ibm, PAGE_SIZE);
    pcache_mark_dirty(ri->dev, ri->start_block + ri->inode_bitmap_start);
    pcache_invalidate(ri->dev, ri->start_block + abs_dblk);
    return 0;
}

int obfs_rw_readlink(struct vfs_inode *ino, char *buf, uint32_t bufsz)
{
    if (!ino || !buf || bufsz == 0) return -22;
    if ((ino->mode & VFS_S_IFMT) != VFS_S_IFLNK) return -22;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)ino->sb->fs_data;
    struct obfs_rw_inode_wrap *iw = (struct obfs_rw_inode_wrap *)ino->fs_data;

    uint32_t blk = iw->disk.base.direct[0];
    if (blk == 0) { buf[0] = '\0'; return 0; }

    uint8_t *b = 0;
    if (rw_read_block(ri, blk, &b) != 0) return -5;

    uint32_t n = (uint32_t)iw->disk.base.size;
    if (n > bufsz - 1) n = bufsz - 1;
    for (uint32_t i = 0; i < n; ++i) buf[i] = (char)b[i];
    buf[n] = '\0';
    return 0;
}

/* ---------- 读 ---------- */
static int64_t rw_read_range(struct obfs_rw_info *ri,
                             struct obfs_inode_rw *ino,
                             uint64_t offset, void *buf, uint64_t count)
{
    if (offset >= ino->base.size) return 0;
    uint64_t avail = ino->base.size - offset;
    if (count > avail) count = avail;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t done = 0;

    while (done < count) {
        uint64_t abs_off = offset + done;
        uint64_t lb = abs_off / PAGE_SIZE;
        uint32_t ib = (uint32_t)(abs_off % PAGE_SIZE);
        uint32_t take = (uint32_t)(PAGE_SIZE - ib);
        if (take > (count - done)) take = (uint32_t)(count - done);

        uint64_t phys = 0;
        int have = (resolve_read(ri, ino, lb, &phys) == 0);

        if (!have) {
            for (uint32_t i = 0; i < take; ++i) dst[done + i] = 0;
        } else {
            uint8_t *b = 0;
            if (rw_read_block(ri, phys, &b) != 0) break;
            for (uint32_t i = 0; i < take; ++i) dst[done + i] = b[ib + i];
        }
        done += take;
    }
    return (int64_t)done;
}

static int64_t rw_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !f->f_inode) return -22;
    struct obfs_rw_info *ri = (struct obfs_rw_info *)f->f_inode->sb->fs_data;
    struct obfs_rw_inode_wrap *iw =
        (struct obfs_rw_inode_wrap *)f->f_inode->fs_data;

    int64_t n = rw_read_range(ri, &iw->disk, f->f_pos, buf, count);
    if (n > 0) f->f_pos += (uint64_t)n;
    return n;
}

/* ---------- 写 ---------- */
/*
 * ★ 关键修复：rw_write_range 使用 g_rw_pb9 替代 4KB 栈变量。
 */
static int64_t rw_write_range(struct obfs_rw_info *ri, struct vfs_inode *vi,
                              struct obfs_rw_inode_wrap *iw,
                              uint64_t offset, const void *buf, uint64_t count)
{
    if (!ri->writable) return -30;

    uint64_t new_size = offset + count;
    if (quota_check_write(&iw->disk.base, new_size) != 0) return -122;

    struct journal_txn *txn = journal_begin(&ri->jnl);
    if (!txn) return -11;

    const uint8_t *src = (const uint8_t *)buf;
    uint64_t done = 0;

    while (done < count) {
        uint64_t abs_off = offset + done;
        uint64_t lb = abs_off / PAGE_SIZE;
        uint32_t ib = (uint32_t)(abs_off % PAGE_SIZE);
        uint32_t take = (uint32_t)(PAGE_SIZE - ib);
        if (take > (count - done)) take = (uint32_t)(count - done);

        uint64_t phys = 0;
        int rc = resolve_write(ri, txn, &iw->disk, lb, &phys);
        if (rc != 0) { journal_abort(&ri->jnl, txn); return rc; }

        uint8_t *b = 0;
        if (rw_read_block(ri, phys, &b) != 0) {
            journal_abort(&ri->jnl, txn);
            return -5;
        }
        uint8_t *tmp = g_rw_pb9;                 /* ★ 静态缓冲 */
        byte_copy(tmp, b, PAGE_SIZE);
        for (uint32_t i = 0; i < take; ++i) tmp[ib + i] = src[done + i];
        journal_add(txn, ri->start_block + phys, tmp);
        byte_copy(b, tmp, PAGE_SIZE);
        pcache_mark_dirty(ri->dev, ri->start_block + phys);

        done += take;
    }

    if (new_size > iw->disk.base.size) {
        iw->disk.base.size = new_size;
        vi->size = new_size;
    }
    rw_log_inode(txn, ri, vi->ino, &iw->disk);

    int rc = journal_commit(&ri->jnl, txn);
    if (rc != 0) return rc;
    return (int64_t)count;
}

static int64_t rw_write(struct vfs_file *f, const void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return -22;
    struct obfs_rw_info *ri = (struct obfs_rw_info *)f->f_inode->sb->fs_data;
    struct obfs_rw_inode_wrap *iw =
        (struct obfs_rw_inode_wrap *)f->f_inode->fs_data;

    int64_t n = rw_write_range(ri, f->f_inode, iw, f->f_pos, buf, count);
    if (n > 0) f->f_pos += (uint64_t)n;
    return n;
}

/* ---------- readdir ---------- */
static int rw_readdir(struct vfs_inode *dir, uint64_t index,
                      struct vfs_dirent *out)
{
    if (!dir || !out) return -22;
    if ((dir->mode & VFS_S_IFMT) != VFS_S_IFDIR) return -20;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)dir->sb->fs_data;
    struct obfs_rw_inode_wrap *dw = (struct obfs_rw_inode_wrap *)dir->fs_data;

    uint64_t seen = 0;
    for (uint32_t i = 0; i < NDIRECT; ++i) {
        if (dw->disk.base.direct[i] == 0) continue;
        uint8_t *b = 0;
        if (rw_read_block(ri, dw->disk.base.direct[i], &b) != 0) continue;
        for (uint32_t off = 0;
             off + sizeof(struct obfs_dirent) <= PAGE_SIZE;
             off += sizeof(struct obfs_dirent)) {
            struct obfs_dirent *de = (struct obfs_dirent *)(b + off);
            if (de->ino == 0) continue;
            if (seen == index) {
                out->ino      = de->ino;
                out->type     = de->type;
                out->name_len = de->name_len;
                for (uint16_t k = 0; k < de->name_len; ++k)
                    out->name[k] = de->name[k];
                out->name[de->name_len] = '\0';
                return 0;
            }
            ++seen;
        }
    }
    return 1;
}

/* ---------- evict / destroy ---------- */
static void rw_evict_inode(struct vfs_inode *inode)
{
    if (!inode) return;
    if (inode->fs_data) kfree(inode->fs_data);
    kfree(inode);
}

static void rw_destroy_sb(struct vfs_superblock *sb)
{
    if (!sb) return;
    if (sb->root) {
        if (sb->root->fs_data) kfree(sb->root->fs_data);
        kfree(sb->root);
    }
    if (sb->fs_data) kfree(sb->fs_data);
    kfree(sb);
}

/* ---------- 挂载 ---------- */
struct vfs_superblock *obfs_rw_mount(struct block_device *dev,
                                     uint64_t start_block,
                                     uint64_t total_blocks)
{
    if (!dev) return 0;

    uint8_t *blk0 = 0;
    if (pcache_get(dev, start_block + 0, &blk0, 0) != 0) {
        serial_printf("[OBFS-RW] cannot read superblock\n");
        return 0;
    }

    const struct obfs_superblock *disk = (const struct obfs_superblock *)blk0;
    if (disk->magic != OBFS_MAGIC) {
        serial_printf("[OBFS-RW] bad magic 0x%x\n", (unsigned)disk->magic);
        return 0;
    }
    if (disk->block_size != PAGE_SIZE) return 0;
    if (disk->total_blocks > total_blocks) return 0;
    if (disk->root_inode >= disk->inode_count) return 0;

    struct obfs_rw_info *ri = (struct obfs_rw_info *)kzalloc(sizeof(*ri));
    if (!ri) return 0;

    ri->dev                = dev;
    ri->start_block        = start_block;
    ri->total_blocks       = disk->total_blocks;
    ri->inode_count        = disk->inode_count;
    ri->block_bitmap_start = disk->block_bitmap_start;
    ri->inode_bitmap_start = disk->inode_bitmap_start;
    ri->inode_table_start  = disk->inode_table_start;
    ri->data_block_start   = disk->data_block_start;
    ri->root_inode         = disk->root_inode;
    spin_lock_init(&ri->lock);

    const struct obfs_superblock_rw *rw =
        (const struct obfs_superblock_rw *)blk0;
    if (rw->rw_version == 1 && rw->journal_size > 0) {
        ri->writable = 1;
        ri->journal_start = rw->journal_start;
        ri->journal_size  = rw->journal_size;

        journal_open(&ri->jnl, dev, start_block + rw->journal_start,
                     rw->journal_size);
        journal_recover(&ri->jnl);
    } else {
        ri->writable = 0;
        serial_printf("[OBFS-RW] image is read-only\n");
    }

    struct vfs_superblock *sb = (struct vfs_superblock *)kzalloc(sizeof(*sb));
    if (!sb) { kfree(ri); return 0; }
    sb->magic        = OBFS_MAGIC;
    sb->block_size   = PAGE_SIZE;
    sb->total_blocks = disk->total_blocks;
    sb->ops          = &g_obfs_rw_ops;
    sb->fs_data      = ri;

    struct vfs_inode *root = 0;
    if (rw_get_inode_vfs(sb, disk->root_inode, &root) != 0) {
        kfree(ri); kfree(sb);
        return 0;
    }
    sb->root = root;

    serial_printf("[OBFS-RW] mounted: blocks=%llu inodes=%llu root=%llu "
                  "writable=%d jsize=%u\n",
                  (unsigned long long)disk->total_blocks,
                  (unsigned long long)disk->inode_count,
                  (unsigned long long)disk->root_inode,
                  ri->writable,
                  (unsigned)ri->journal_size);
    return sb;
}

void obfs_rw_umount(struct vfs_superblock *sb)
{
    if (!sb) return;
    struct obfs_rw_info *ri = (struct obfs_rw_info *)sb->fs_data;
    if (ri && ri->writable) {
        pcache_sync();
    }
    rw_destroy_sb(sb);
}

int obfs_rw_is_writable(struct vfs_superblock *sb)
{
    if (!sb) return 0;
    struct obfs_rw_info *ri = (struct obfs_rw_info *)sb->fs_data;
    return ri && ri->writable;
}

struct journal *obfs_rw_journal(struct vfs_superblock *sb)
{
    if (!sb) return 0;
    struct obfs_rw_info *ri = (struct obfs_rw_info *)sb->fs_data;
    return &ri->jnl;
}

int obfs_rw_sync(struct vfs_superblock *sb)
{
    if (!sb) return -22;
    return pcache_sync();
}

/* ---------- 配额辅助 ---------- */
uint64_t obfs_inode_get_quota(const struct obfs_inode *ino)
{
    const struct obfs_inode_rw *r = (const struct obfs_inode_rw *)ino;
    return r->quota_limit;
}

int obfs_inode_set_quota(struct obfs_inode *ino, uint64_t limit)
{
    struct obfs_inode_rw *r = (struct obfs_inode_rw *)ino;
    r->quota_limit = limit;
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/block/obfs_rw.c 结束===*/