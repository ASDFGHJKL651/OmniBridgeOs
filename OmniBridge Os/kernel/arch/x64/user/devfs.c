/*===OmniBridgeOs/kernel/arch/x64/user/devfs.c===*/
/*
 * 最小 /dev 虚拟文件系统（第 19 步）。
 *
 * 设备：
 *   /dev/null     —— 读返回 0；写丢弃。
 *   /dev/zero     —— 读返回全 0；写丢弃。
 *   /dev/random   —— 读返回 rng_bytes 结果。
 *   /dev/urandom  —— 同 random。
 *   /dev/tty      —— 读写映射到串口。
 *
 * 人工必须审查：
 *   - /dev/random 必须真的调用 rng_bytes，不能返回常量。
 *   - /dev/null 写必须丢弃，不落盘。
 *   - /dev/tty 与 fd 0/1/2 共享同一底层。
 */
#include "vfs.h"
#include "task.h"
#include "sched.h"
#include "serial.h"
#include "kmalloc.h"
#include "rng.h"

enum devfs_type {
    DVT_NULL,
    DVT_ZERO,
    DVT_RANDOM,
    DVT_URANDOM,
    DVT_TTY,
};

struct devfs_node {
    char              name[32];
    enum devfs_type   type;
    struct devfs_node *next;
};

struct devfs_priv {
    struct devfs_node *node;
};

/* ---------- 静态设备表 ---------- */
static struct devfs_node g_dev_tty = {
    "tty", DVT_TTY, 0
};
static struct devfs_node g_dev_urandom = {
    "urandom", DVT_URANDOM, &g_dev_tty
};
static struct devfs_node g_dev_random = {
    "random", DVT_RANDOM, &g_dev_urandom
};
static struct devfs_node g_dev_zero = {
    "zero", DVT_ZERO, &g_dev_random
};
static struct devfs_node g_dev_null = {
    "null", DVT_NULL, &g_dev_zero
};
static struct devfs_node g_dev_root = {
    "", 0xFF, &g_dev_null
};

static struct devfs_priv *priv_of(struct vfs_inode *i)
{
    return i ? (struct devfs_priv *)i->fs_data : 0;
}

static int devfs_lookup(struct vfs_inode *dir, const char *name,
                        struct vfs_inode **out)
{
    *out = 0;
    if (!dir || !name) return OB_EINVAL;
    struct devfs_priv *dp = priv_of(dir);
    if (!dp || dp->node->type != 0xFF) return OB_ENOTDIR;

    struct devfs_node *found = 0;
    for (struct devfs_node *n = dp->node->next; n; n = n->next) {
        const char *a = n->name;
        const char *b = name;
        int eq = 1;
        while (*a && *b) { if (*a != *b) { eq = 0; break; } ++a; ++b; }
        if (eq && *a == '\0' && *b == '\0') { found = n; break; }
    }
    if (!found) return OB_ENOENT;

    struct vfs_inode *ni = (struct vfs_inode *)kzalloc(sizeof(*ni));
    if (!ni) return OB_ENOMEM;
    struct devfs_priv *np = (struct devfs_priv *)kzalloc(sizeof(*np));
    if (!np) { kfree(ni); return OB_ENOMEM; }
    np->node = found;

    ni->mode     = VFS_S_IFCHR | 0666;
    ni->sb       = dir->sb;
    ni->ops      = dir->ops;
    ni->fs_data  = np;
    ni->refcount = 1;

    *out = ni;
    return 0;
}

static int devfs_readdir(struct vfs_inode *dir, uint64_t index,
                         struct vfs_dirent *out)
{
    if (!dir || !out) return OB_EINVAL;
    struct devfs_priv *dp = priv_of(dir);
    if (!dp || dp->node->type != 0xFF) return OB_ENOTDIR;

    uint64_t i = 0;
    for (struct devfs_node *n = dp->node->next; n; n = n->next) {
        if (i == index) {
            out->ino      = 0;
            out->type     = VFS_FT_CHR;
            out->name_len = 0;
            const char *s = n->name;
            while (*s && out->name_len < VFS_NAME_MAX - 1) {
                out->name[out->name_len++] = *s++;
            }
            out->name[out->name_len] = '\0';
            return 0;
        }
        ++i;
    }
    return 1;
}

static int64_t devfs_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;
    struct devfs_priv *dp = priv_of(f->f_inode);
    if (!dp) return OB_EIO;

    uint8_t *dst = (uint8_t *)buf;

    switch (dp->node->type) {
    case DVT_NULL:
        return 0;
    case DVT_ZERO:
        for (uint64_t i = 0; i < count; ++i) dst[i] = 0;
        return (int64_t)count;
    case DVT_RANDOM:
    case DVT_URANDOM:
        if (rng_bytes(dst, count) != 0) return OB_EIO;
        return (int64_t)count;
    case DVT_TTY:
        /* 简化为：读返回 0（EOF） */
        return 0;
    default:
        return OB_EIO;
    }
}

static int64_t devfs_write(struct vfs_file *f, const void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;
    struct devfs_priv *dp = priv_of(f->f_inode);
    if (!dp) return OB_EIO;

    switch (dp->node->type) {
    case DVT_NULL:
    case DVT_ZERO:
    case DVT_RANDOM:
    case DVT_URANDOM:
        return (int64_t)count;   /* 丢弃 */
    case DVT_TTY: {
        const char *p = (const char *)buf;
        for (uint64_t i = 0; i < count; ++i) serial_putc(p[i]);
        return (int64_t)count;
    }
    default:
        return OB_EIO;
    }
}

static void devfs_evict_inode(struct vfs_inode *inode)
{
    if (!inode) return;
    if (inode->fs_data) kfree(inode->fs_data);
    kfree(inode);
}

static void devfs_destroy_sb(struct vfs_superblock *sb)
{
    if (!sb) return;
    if (sb->root) {
        if (sb->root->fs_data) kfree(sb->root->fs_data);
        kfree(sb->root);
    }
    kfree(sb);
}

static const struct vfs_operations g_devfs_ops = {
    .lookup      = devfs_lookup,
    .read        = devfs_read,
    .write       = devfs_write,
    .readdir     = devfs_readdir,
    .evict_inode = devfs_evict_inode,
    .destroy_sb  = devfs_destroy_sb,
};

void devfs_init(void)
{
    serial_printf("[DEVFS] init\n");
}

struct vfs_superblock *devfs_mount(void)
{
    struct vfs_superblock *sb =
        (struct vfs_superblock *)kzalloc(sizeof(*sb));
    if (!sb) return 0;

    struct vfs_inode *root =
        (struct vfs_inode *)kzalloc(sizeof(*root));
    if (!root) { kfree(sb); return 0; }

    struct devfs_priv *rp = (struct devfs_priv *)kzalloc(sizeof(*rp));
    if (!rp) { kfree(root); kfree(sb); return 0; }
    rp->node = &g_dev_root;

    root->ino      = 1;
    root->mode     = VFS_S_IFDIR | 0755;
    root->links    = 1;
    root->sb       = sb;
    root->ops      = &g_devfs_ops;
    root->fs_data  = rp;
    root->refcount = 1;

    sb->magic        = 0x44455646u;   /* "DEVF" */
    sb->block_size   = 4096;
    sb->ops          = &g_devfs_ops;
    sb->root         = root;

    serial_printf("[DEVFS] mounted\n");
    return sb;
}
/*===OmniBridgeOs/kernel/arch/x64/user/devfs.c 结束===*/