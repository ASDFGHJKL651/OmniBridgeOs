/*===OmniBridgeOs/kernel/arch/x64/user/sysfs.c===*/
/*
 * 最小 /sys 虚拟文件系统（第 19 步）。
 *
 * 路径（只读）：
 *   /sys/class/net/lo/address       -> "00:00:00:00:00:00\n"
 *   /sys/class/net/eth0/address     -> "52:54:00:12:34:56\n"
 *   /sys/devices/system/cpu/online  -> "0\n"
 *
 * 人工必须审查：
 *   - 内容为静态常量；本步不反映真实硬件状态（后续步骤扩展）。
 *   - 只读挂载（不提供 write）。
 */
#include "vfs.h"
#include "task.h"
#include "serial.h"
#include "kmalloc.h"

struct sysfs_node {
    char     name[32];
    int      is_dir;
    const char *content;    /* 目录时为 NULL */
    struct sysfs_node *children;
    struct sysfs_node *next;
};

struct sysfs_priv {
    struct sysfs_node *node;
};

/* ---------- 静态目录树 ---------- */
static struct sysfs_node g_online = {
    "online", 0, "0\n", 0, 0
};
static struct sysfs_node g_cpu = {
    "cpu", 1, 0, &g_online, 0
};
static struct sysfs_node g_system = {
    "system", 1, 0, &g_cpu, 0
};
static struct sysfs_node g_devices = {
    "devices", 1, 0, &g_system, 0
};

static struct sysfs_node g_addr_eth0 = {
    "address", 0, "52:54:00:12:34:56\n", 0, 0
};
static struct sysfs_node g_eth0 = {
    "eth0", 1, 0, &g_addr_eth0, 0
};
static struct sysfs_node g_addr_lo = {
    "address", 0, "00:00:00:00:00:00\n", 0, &g_eth0
};
static struct sysfs_node g_lo = {
    "lo", 1, 0, &g_addr_lo, 0
};
static struct sysfs_node g_net = {
    "net", 1, 0, &g_lo, 0
};
static struct sysfs_node g_class = {
    "class", 1, 0, &g_net, 0
};
static struct sysfs_node g_root = {
    "", 1, 0, &g_class, &g_devices
};

static struct sysfs_priv *priv_of(struct vfs_inode *i)
{
    return i ? (struct sysfs_priv *)i->fs_data : 0;
}

static struct sysfs_node *find_child(struct sysfs_node *dir, const char *name)
{
    if (!dir) return 0;
    for (struct sysfs_node *n = dir->children; n; n = n->next) {
        const char *a = n->name;
        const char *b = name;
        int eq = 1;
        while (*a && *b) { if (*a != *b) { eq = 0; break; } ++a; ++b; }
        if (eq && *a == '\0' && *b == '\0') return n;
    }
    return 0;
}

static int sysfs_lookup(struct vfs_inode *dir, const char *name,
                        struct vfs_inode **out)
{
    *out = 0;
    if (!dir || !name) return OB_EINVAL;
    struct sysfs_priv *dp = priv_of(dir);
    if (!dp || !dp->node->is_dir) return OB_ENOTDIR;

    struct sysfs_node *found = find_child(dp->node, name);
    if (!found) return OB_ENOENT;

    struct vfs_inode *ni = (struct vfs_inode *)kzalloc(sizeof(*ni));
    if (!ni) return OB_ENOMEM;
    struct sysfs_priv *np = (struct sysfs_priv *)kzalloc(sizeof(*np));
    if (!np) { kfree(ni); return OB_ENOMEM; }
    np->node = found;

    ni->mode     = found->is_dir
                   ? (uint16_t)(VFS_S_IFDIR | 0555)
                   : (uint16_t)(VFS_S_IFREG | 0444);
    ni->sb       = dir->sb;
    ni->ops      = dir->ops;
    ni->fs_data  = np;
    ni->refcount = 1;

    *out = ni;
    return 0;
}

static int sysfs_readdir(struct vfs_inode *dir, uint64_t index,
                         struct vfs_dirent *out)
{
    if (!dir || !out) return OB_EINVAL;
    struct sysfs_priv *dp = priv_of(dir);
    if (!dp || !dp->node->is_dir) return OB_ENOTDIR;

    uint64_t i = 0;
    for (struct sysfs_node *n = dp->node->children; n; n = n->next) {
        if (i == index) {
            out->ino      = 0;
            out->type     = n->is_dir ? VFS_FT_DIR : VFS_FT_REG;
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

static int64_t sysfs_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;
    struct sysfs_priv *sp = priv_of(f->f_inode);
    if (!sp || !sp->node->content) return OB_EIO;

    const char *s = sp->node->content;
    uint64_t total = 0;
    while (s[total]) ++total;

    if (f->f_pos >= total) return 0;
    uint64_t avail = total - f->f_pos;
    uint64_t take = count < avail ? count : avail;
    uint8_t *dst = (uint8_t *)buf;
    for (uint64_t i = 0; i < take; ++i) dst[i] = (uint8_t)s[f->f_pos + i];
    f->f_pos += take;
    return (int64_t)take;
}

static int64_t sysfs_write(struct vfs_file *f, const void *buf, uint64_t count)
{
    (void)f; (void)buf; (void)count;
    return OB_EROFS;
}

static void sysfs_evict_inode(struct vfs_inode *inode)
{
    if (!inode) return;
    if (inode->fs_data) kfree(inode->fs_data);
    kfree(inode);
}

static void sysfs_destroy_sb(struct vfs_superblock *sb)
{
    if (!sb) return;
    if (sb->root) {
        if (sb->root->fs_data) kfree(sb->root->fs_data);
        kfree(sb->root);
    }
    kfree(sb);
}

static const struct vfs_operations g_sysfs_ops = {
    .lookup      = sysfs_lookup,
    .read        = sysfs_read,
    .write       = sysfs_write,
    .readdir     = sysfs_readdir,
    .evict_inode = sysfs_evict_inode,
    .destroy_sb  = sysfs_destroy_sb,
};

void sysfs_init(void)
{
    serial_printf("[SYSFS] init\n");
}

struct vfs_superblock *sysfs_mount(void)
{
    struct vfs_superblock *sb =
        (struct vfs_superblock *)kzalloc(sizeof(*sb));
    if (!sb) return 0;

    struct vfs_inode *root =
        (struct vfs_inode *)kzalloc(sizeof(*root));
    if (!root) { kfree(sb); return 0; }

    struct sysfs_priv *rp = (struct sysfs_priv *)kzalloc(sizeof(*rp));
    if (!rp) { kfree(root); kfree(sb); return 0; }
    rp->node = &g_root;

    root->ino      = 1;
    root->mode     = VFS_S_IFDIR | 0555;
    root->links    = 1;
    root->sb       = sb;
    root->ops      = &g_sysfs_ops;
    root->fs_data  = rp;
    root->refcount = 1;

    sb->magic        = 0x53595346u;   /* "SYSF" */
    sb->block_size   = 4096;
    sb->ops          = &g_sysfs_ops;
    sb->root         = root;

    serial_printf("[SYSFS] mounted\n");
    return sb;
}
/*===OmniBridgeOs/kernel/arch/x64/user/sysfs.c 结束===*/