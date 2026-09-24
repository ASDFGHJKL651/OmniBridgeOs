/*===OmniBridgeOs/kernel/arch/x64/tmpfs.c===*/
#include "tmpfs.h"
#include "kmalloc.h"
#include "serial.h"
#include "rng.h"

/*
 * tmpfs —— 内存文件系统（第 11 步，第 15 步修改）。
 */

struct tmpfs_node {
    char name[VFS_NAME_MAX];
    uint32_t name_len;
    uint32_t _pad;
    struct vfs_inode *inode;
    struct tmpfs_node *next;
};

struct tmpfs_inode_info {
    struct tmpfs_node *children;
    uint64_t parent_ino;
    void    *data;
    uint64_t capacity;
    uint64_t size;
};

struct tmpfs_sb_info {
    uint64_t max_size;
    uint64_t used;
    uint64_t next_ino;
};

static int tmpfs_lookup(struct vfs_inode *dir, const char *name,
                        struct vfs_inode **out);
static int tmpfs_create(struct vfs_inode *dir, const char *name,
                        uint16_t mode, struct vfs_inode **out);
static int tmpfs_unlink(struct vfs_inode *dir, const char *name);
static int tmpfs_mkdir(struct vfs_inode *dir, const char *name,
                       uint16_t mode, struct vfs_inode **out);
static int tmpfs_rmdir(struct vfs_inode *dir, const char *name);
static int64_t tmpfs_file_read(struct vfs_file *f, void *buf, uint64_t count);
static int64_t tmpfs_file_write(struct vfs_file *f, const void *buf, uint64_t count);
static int tmpfs_readdir(struct vfs_inode *dir, uint64_t index,
                         struct vfs_dirent *out);
static int tmpfs_open(struct vfs_inode *inode, struct vfs_file *file);
static int tmpfs_close(struct vfs_file *file);
static int tmpfs_truncate(struct vfs_inode *inode, uint64_t size);
static void tmpfs_evict_inode(struct vfs_inode *inode);
static void tmpfs_destroy_sb(struct vfs_superblock *sb);

static const struct vfs_operations g_tmpfs_ops = {
    .lookup       = tmpfs_lookup,
    .create       = tmpfs_create,
    .unlink       = tmpfs_unlink,
    .mkdir        = tmpfs_mkdir,
    .rmdir        = tmpfs_rmdir,
    .read         = tmpfs_file_read,
    .write        = tmpfs_file_write,
    .readdir      = tmpfs_readdir,
    .open         = tmpfs_open,
    .close        = tmpfs_close,
    .truncate     = tmpfs_truncate,
    .evict_inode  = tmpfs_evict_inode,
    .destroy_sb   = tmpfs_destroy_sb,
};

/* ============================================================
 * 工具函数
 * ============================================================ */

static size_t str_len(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static int str_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static void str_copy_n(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    if (cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static uint64_t children_count(struct tmpfs_inode_info *info)
{
    uint64_t n = 0;
    for (struct tmpfs_node *t = info ? info->children : 0; t; t = t->next) ++n;
    return n;
}

/* ============================================================
 * inode 分配 / 释放
 * ============================================================ */

static struct vfs_inode *tmpfs_alloc_inode(struct vfs_superblock *sb,
                                           uint16_t mode,
                                           uint64_t parent_ino)
{
    struct tmpfs_sb_info *sbi = (struct tmpfs_sb_info *)sb->fs_data;
    if (!sbi) return 0;

    struct vfs_inode *i = (struct vfs_inode *)kzalloc(sizeof(*i));
    if (!i) return 0;

    struct tmpfs_inode_info *info =
        (struct tmpfs_inode_info *)kzalloc(sizeof(*info));
    if (!info) { kfree(i); return 0; }

    i->ino       = sbi->next_ino++;
    i->mode      = mode;
    i->links     = 1;
    i->uid       = 0;
    i->gid       = 0;
    i->size      = 0;
    i->blocks    = 0;
    i->atime     = 0;
    i->mtime     = 0;
    i->ctime     = 0;
    i->flags     = 0;
    i->sb        = sb;
    i->ops       = &g_tmpfs_ops;
    i->fs_data   = info;
    i->refcount  = 0;

    info->parent_ino = parent_ino;
    info->children   = 0;
    info->data       = 0;
    info->capacity   = 0;
    info->size       = 0;

    return i;
}

static void tmpfs_free_inode(struct vfs_inode *inode)
{
    if (!inode) return;

    struct tmpfs_inode_info *info =
        (struct tmpfs_inode_info *)inode->fs_data;

    if (info) {
        struct tmpfs_node *n = info->children;
        while (n) {
            struct tmpfs_node *next = n->next;
            if (n->inode) tmpfs_free_inode(n->inode);
            kfree(n);
            n = next;
        }
        if (info->data) kfree(info->data);
        kfree(info);
    }

    kfree(inode);
}

/* ============================================================
 * children 链管理
 * ============================================================ */

static struct tmpfs_node *tmpfs_find_child(struct tmpfs_inode_info *dir_info,
                                           const char *name)
{
    if (!dir_info) return 0;
    for (struct tmpfs_node *n = dir_info->children; n; n = n->next) {
        if (str_eq(n->name, name)) return n;
    }
    return 0;
}

static int tmpfs_add_child(struct tmpfs_inode_info *dir_info,
                           const char *name,
                           struct vfs_inode *inode)
{
    if (!dir_info) return OB_EIO;

    struct tmpfs_node *n =
        (struct tmpfs_node *)kzalloc(sizeof(*n));
    if (!n) return OB_ENOMEM;

    str_copy_n(n->name, name, VFS_NAME_MAX);
    n->name_len = (uint32_t)str_len(n->name);
    n->inode    = inode;
    n->next     = 0;

    if (!dir_info->children) {
        dir_info->children = n;
    } else {
        struct tmpfs_node *t = dir_info->children;
        while (t->next) t = t->next;
        t->next = n;
    }

    serial_printf("[TMPFS-DBG] add_child child='%s' children_cnt=%llu\n",
                  n->name,
                  (unsigned long long)children_count(dir_info));
    return 0;
}

static struct tmpfs_node *tmpfs_remove_child(struct tmpfs_inode_info *dir_info,
                                             const char *name)
{
    if (!dir_info) return 0;
    struct tmpfs_node **pp = &dir_info->children;
    while (*pp && !str_eq((*pp)->name, name)) pp = &(*pp)->next;
    if (!*pp) return 0;
    struct tmpfs_node *n = *pp;
    *pp = n->next;
    n->next = 0;
    return n;
}

/* ============================================================
 * 操作表实现
 * ============================================================ */

static int tmpfs_lookup(struct vfs_inode *dir, const char *name,
                        struct vfs_inode **out)
{
    if (!dir || !name || !out) return OB_EINVAL;
    *out = 0;
    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;

    struct tmpfs_inode_info *info =
        (struct tmpfs_inode_info *)dir->fs_data;
    struct tmpfs_node *n = tmpfs_find_child(info, name);
    if (!n) {
        serial_printf("[TMPFS-DBG] lookup dir_ino=%llu name='%s' -> ENOENT "
                      "(children_cnt=%llu)\n",
                      (unsigned long long)dir->ino, name,
                      (unsigned long long)children_count(info));
        return OB_ENOENT;
    }
    serial_printf("[TMPFS-DBG] lookup dir_ino=%llu name='%s' -> ino=%llu\n",
                  (unsigned long long)dir->ino, name,
                  (unsigned long long)n->inode->ino);
    *out = n->inode;
    return 0;
}

static int tmpfs_create(struct vfs_inode *dir, const char *name,
                        uint16_t mode, struct vfs_inode **out)
{
    if (!dir || !name || !out) return OB_EINVAL;
    *out = 0;
    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;
    if (name[0] == '\0') return OB_EINVAL;

    struct tmpfs_inode_info *dir_info =
        (struct tmpfs_inode_info *)dir->fs_data;
    if (!dir_info) return OB_EIO;

    if (tmpfs_find_child(dir_info, name)) return OB_EEXIST;

    uint16_t ftype = (uint16_t)(mode & VFS_S_IFMT);
    if (ftype == 0) ftype = VFS_S_IFREG;
    if (ftype != VFS_S_IFREG) return OB_EINVAL;

    struct vfs_inode *ino = tmpfs_alloc_inode(dir->sb, mode, dir->ino);
    if (!ino) return OB_ENOMEM;

    int rc = tmpfs_add_child(dir_info, name, ino);
    if (rc != 0) {
        tmpfs_free_inode(ino);
        return rc;
    }

    serial_printf("[TMPFS-DBG] create name='%s' parent_ino=%llu new_ino=%llu\n",
                  name,
                  (unsigned long long)dir->ino,
                  (unsigned long long)ino->ino);
    *out = ino;
    return 0;
}

static int tmpfs_mkdir(struct vfs_inode *dir, const char *name,
                       uint16_t mode, struct vfs_inode **out)
{
    if (!dir || !name || !out) return OB_EINVAL;
    *out = 0;
    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;
    if (name[0] == '\0') return OB_EINVAL;

    struct tmpfs_inode_info *dir_info =
        (struct tmpfs_inode_info *)dir->fs_data;
    if (!dir_info) return OB_EIO;

    if (tmpfs_find_child(dir_info, name)) return OB_EEXIST;

    uint16_t m = (uint16_t)(VFS_S_IFDIR | (mode & 0x0FFF));
    struct vfs_inode *ino = tmpfs_alloc_inode(dir->sb, m, dir->ino);
    if (!ino) return OB_ENOMEM;

    int rc = tmpfs_add_child(dir_info, name, ino);
    if (rc != 0) {
        tmpfs_free_inode(ino);
        return rc;
    }

    serial_printf("[TMPFS-DBG] mkdir name='%s' parent_ino=%llu new_ino=%llu\n",
                  name,
                  (unsigned long long)dir->ino,
                  (unsigned long long)ino->ino);
    *out = ino;
    return 0;
}

static int tmpfs_unlink(struct vfs_inode *dir, const char *name)
{
    if (!dir || !name) return OB_EINVAL;
    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;

    struct tmpfs_inode_info *dir_info =
        (struct tmpfs_inode_info *)dir->fs_data;
    struct tmpfs_node *n = tmpfs_find_child(dir_info, name);
    if (!n) return OB_ENOENT;
    if (VFS_S_ISDIR(n->inode->mode)) return OB_EISDIR;

    struct tmpfs_node *removed = tmpfs_remove_child(dir_info, name);
    if (!removed) return OB_ENOENT;
    tmpfs_free_inode(removed->inode);
    kfree(removed);
    return 0;
}

static int tmpfs_rmdir(struct vfs_inode *dir, const char *name)
{
    if (!dir || !name) return OB_EINVAL;
    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;

    struct tmpfs_inode_info *dir_info =
        (struct tmpfs_inode_info *)dir->fs_data;
    struct tmpfs_node *n = tmpfs_find_child(dir_info, name);
    if (!n) return OB_ENOENT;
    if (!VFS_S_ISDIR(n->inode->mode)) return OB_ENOTDIR;

    struct tmpfs_inode_info *child_info =
        (struct tmpfs_inode_info *)n->inode->fs_data;
    if (child_info && child_info->children) return OB_ENOTEMPTY;

    struct tmpfs_node *removed = tmpfs_remove_child(dir_info, name);
    if (!removed) return OB_ENOENT;
    tmpfs_free_inode(removed->inode);
    kfree(removed);
    return 0;
}

/* ============================================================
 * 数据读写
 * ============================================================ */

static int64_t tmpfs_file_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;

    struct tmpfs_inode_info *info =
        (struct tmpfs_inode_info *)f->f_inode->fs_data;
    if (!info) return OB_EIO;

    uint64_t pos = f->f_pos;
    uint64_t sz  = info->size;
    if (pos >= sz) return 0;

    uint64_t avail = sz - pos;
    if (count > avail) count = avail;
    if (count == 0) return 0;

    const uint8_t *src = (const uint8_t *)info->data;
    uint8_t *dst = (uint8_t *)buf;
    for (uint64_t i = 0; i < count; ++i) dst[i] = src[pos + i];

    f->f_pos = pos + count;
    return (int64_t)count;
}

static int64_t tmpfs_file_write(struct vfs_file *f, const void *buf,
                                uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;

    struct tmpfs_inode_info *info =
        (struct tmpfs_inode_info *)f->f_inode->fs_data;
    if (!info) return OB_EIO;

    struct tmpfs_sb_info *sbi =
        (struct tmpfs_sb_info *)f->f_inode->sb->fs_data;
    if (!sbi) return OB_EIO;

    uint64_t pos  = f->f_pos;
    uint64_t need = pos + count;
    if (need > sbi->max_size) return OB_ENOMEM;

    if (need > info->capacity) {
        uint64_t newcap = info->capacity ? info->capacity : 64;
        while (newcap < need) newcap *= 2;
        if (newcap > sbi->max_size) newcap = sbi->max_size;
        if (newcap < need) return OB_ENOMEM;

        void *newbuf = kmalloc((size_t)newcap);
        if (!newbuf) return OB_ENOMEM;

        uint8_t *nd = (uint8_t *)newbuf;
        const uint8_t *od = (const uint8_t *)info->data;
        for (uint64_t i = 0; i < info->size; ++i) {
            nd[i] = od ? od[i] : 0;
        }
        if (info->data) kfree(info->data);
        info->data = newbuf;
        info->capacity = newcap;
    }

    uint8_t *d = (uint8_t *)info->data;
    for (uint64_t i = info->size; i < pos; ++i) d[i] = 0;

    const uint8_t *s = (const uint8_t *)buf;
    for (uint64_t i = 0; i < count; ++i) d[pos + i] = s[i];

    if (need > info->size) info->size = need;
    f->f_pos = need;
    f->f_inode->size = info->size;
    f->f_inode->blocks = (info->size + 511) / 512;

    return (int64_t)count;
}

static int tmpfs_truncate(struct vfs_inode *inode, uint64_t size)
{
    if (!inode) return OB_EINVAL;

    struct tmpfs_inode_info *info =
        (struct tmpfs_inode_info *)inode->fs_data;
    if (!info) return OB_EIO;
    if (size > info->capacity) return OB_ENOMEM;

    info->size = size;
    inode->size = size;
    inode->blocks = (size + 511) / 512;
    return 0;
}

/* ============================================================
 * 目录遍历
 * ============================================================ */
static int tmpfs_readdir(struct vfs_inode *dir, uint64_t index,
                         struct vfs_dirent *out)
{
    if (!dir || !out) return OB_EINVAL;
    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;

    struct tmpfs_inode_info *info =
        (struct tmpfs_inode_info *)dir->fs_data;
    if (!info) return OB_EIO;

    uint64_t i = 0;
    for (struct tmpfs_node *n = info->children; n; n = n->next) {
        if (i == index) {
            out->ino  = n->inode ? n->inode->ino : 0;
            out->type = n->inode ? vfs_mode_to_dt(n->inode->mode)
                                 : VFS_FT_UNKNOWN;

            uint32_t len = n->name_len;
            if (len > VFS_NAME_MAX - 1) len = VFS_NAME_MAX - 1;
            for (uint32_t k = 0; k < len; ++k) out->name[k] = n->name[k];
            out->name[len] = '\0';
            out->name_len = len;

            serial_printf("[TMPFS-DBG] readdir dir_ino=%llu idx=%llu "
                          "children_cnt=%llu -> name='%s'\n",
                          (unsigned long long)dir->ino,
                          (unsigned long long)index,
                          (unsigned long long)children_count(info),
                          out->name);
            return 0;
        }
        ++i;
    }

    serial_printf("[TMPFS-DBG] readdir dir_ino=%llu idx=%llu "
                  "children_cnt=%llu -> ENOENT\n",
                  (unsigned long long)dir->ino,
                  (unsigned long long)index,
                  (unsigned long long)children_count(info));
    return OB_ENOENT;
}

static int tmpfs_open(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

static int tmpfs_close(struct vfs_file *file)
{
    (void)file;
    return 0;
}

static void tmpfs_evict_inode(struct vfs_inode *inode)
{
    if (!inode) return;
    tmpfs_free_inode(inode);
}

static void tmpfs_destroy_sb(struct vfs_superblock *sb)
{
    if (!sb) return;

    if (sb->root) {
        tmpfs_free_inode(sb->root);
        sb->root = 0;
    }

    if (sb->fs_data) {
        kfree(sb->fs_data);
        sb->fs_data = 0;
    }

    kfree(sb);
}

/* ============================================================
 * 公共 API
 * ============================================================ */

struct vfs_superblock *tmpfs_mount(const char *name, uint64_t max_size)
{
    struct vfs_superblock *sb =
        (struct vfs_superblock *)kzalloc(sizeof(*sb));
    if (!sb) return 0;

    struct tmpfs_sb_info *sbi =
        (struct tmpfs_sb_info *)kzalloc(sizeof(*sbi));
    if (!sbi) { kfree(sb); return 0; }

    sbi->max_size = max_size ? max_size : (64ULL * 1024ULL * 1024ULL);
    sbi->used     = 0;
    sbi->next_ino = 1;

    struct vfs_inode *root =
        (struct vfs_inode *)kzalloc(sizeof(*root));
    if (!root) { kfree(sbi); kfree(sb); return 0; }

    struct tmpfs_inode_info *root_info =
        (struct tmpfs_inode_info *)kzalloc(sizeof(*root_info));
    if (!root_info) { kfree(root); kfree(sbi); kfree(sb); return 0; }

    root->ino      = sbi->next_ino++;
    root->mode     = VFS_S_IFDIR | 0755;
    root->links    = 1;
    root->sb       = sb;
    root->ops      = &g_tmpfs_ops;
    root->fs_data  = root_info;
    root->refcount = 0;

    root_info->parent_ino = root->ino;
    root_info->children   = 0;

    sb->magic         = TMPFS_MAGIC;
    sb->block_size    = 4096;
    sb->total_blocks  = sbi->max_size / 4096;
    sb->free_blocks   = sb->total_blocks;
    sb->root          = root;
    sb->ops           = &g_tmpfs_ops;
    sb->fs_data       = sbi;
    sb->next          = 0;
    sb->mount_path[0] = '\0';

    spin_lock_init(&sb->lock);

    serial_printf("[TMPFS] mounted '%s' max=%llu bytes root_ino=%llu\n",
                  name ? name : "tmpfs",
                  (unsigned long long)sbi->max_size,
                  (unsigned long long)root->ino);
    return sb;
}

void tmpfs_umount(struct vfs_superblock *sb)
{
    tmpfs_destroy_sb(sb);
}

int tmpfs_create_priv0_tree(struct vfs_superblock **out)
{
    if (!out) return OB_EINVAL;
    *out = 0;

    struct vfs_superblock *sb = tmpfs_mount("priv0", 64ULL * 1024 * 1024);
    if (!sb) return OB_ENOMEM;

    static const char *names[3] = { "bin", "tmp", "home" };
    for (int i = 0; i < 3; ++i) {
        struct vfs_inode *d = 0;
        int rc = tmpfs_mkdir(sb->root, names[i],
                             (uint16_t)(VFS_S_IFDIR | 0755), &d);
        if (rc != 0) {
            tmpfs_destroy_sb(sb);
            return rc;
        }
    }

    *out = sb;
    return 0;
}

/* ============================================================
 * ★ 第 15 步修改：随机后缀 + 确保 /tmp 存在
 * ============================================================ */
int tmpfs_create_priv1_dir(struct vfs_superblock *sb, uint64_t pid,
                           char *out_path, uint64_t out_size)
{
    if (!sb || !out_path || out_size < 32) return OB_EINVAL;
    if (!sb->root) return OB_EIO;

    /* 1) 确保 /tmp 存在 */
    struct vfs_inode *tmp = 0;
    int rc = tmpfs_lookup(sb->root, "tmp", &tmp);
    if (rc != 0 || !tmp) {
        rc = tmpfs_mkdir(sb->root, "tmp",
                         (uint16_t)(VFS_S_IFDIR | 0777), &tmp);
        if (rc != 0 && rc != OB_EEXIST) return rc;
        if (rc == OB_EEXIST) {
            rc = tmpfs_lookup(sb->root, "tmp", &tmp);
            if (rc != 0 || !tmp) return rc ? rc : OB_ENOENT;
        }
    }

    /* 2) 生成 8 字符随机后缀 */
    uint8_t rnd[4];
    char suffix[9];
    if (rng_bytes(rnd, 4) == 0) {
        const char *alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        for (int i = 0; i < 8; ++i) suffix[i] = alphabet[rnd[i & 3] % 32];
    } else {
        uint32_t v = (uint32_t)(pid * 2654435761u);
        const char *hex = "0123456789abcdef";
        for (int i = 0; i < 8; ++i) suffix[i] = hex[(v >> (i * 4)) & 0xF];
    }
    suffix[8] = '\0';

    /* 3) 构造叶子名：priv1_<pid>_<suffix> */
    char leaf[VFS_NAME_MAX];
    int i = 0;
    const char *pfx = "priv1_";
    while (pfx[i] && i < VFS_NAME_MAX - 20) { leaf[i] = pfx[i]; ++i; }
    {
        char num[24];
        int nd = 0;
        uint64_t v = pid;
        if (v == 0) num[nd++] = '0';
        while (v && nd < (int)sizeof(num) - 1) {
            num[nd++] = (char)('0' + (v % 10));
            v /= 10;
        }
        for (int k = nd - 1; k >= 0 && i < VFS_NAME_MAX - 12; --k)
            leaf[i++] = num[k];
    }
    if (i < VFS_NAME_MAX - 11) leaf[i++] = '_';
    for (int k = 0; k < 8 && i < VFS_NAME_MAX - 3; ++k) leaf[i++] = suffix[k];
    leaf[i] = '\0';

    /* 4) 创建目录（若已存在则复用） */
    struct vfs_inode *newdir = 0;
    rc = tmpfs_mkdir(tmp, leaf, (uint16_t)(VFS_S_IFDIR | 0700), &newdir);
    if (rc != 0 && rc != OB_EEXIST) return rc;

    /* 5) 输出完整路径：/tmp/<leaf>/ */
    uint32_t j = 0;
    const char *pre2 = "/tmp/";
    for (uint32_t k = 0; pre2[k] && j < out_size - 2; ++k)
        out_path[j++] = pre2[k];
    for (uint32_t k = 0; leaf[k] && j < out_size - 2; ++k)
        out_path[j++] = leaf[k];
    out_path[j++] = '/';
    out_path[j] = '\0';
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/tmpfs.c 结束===*/