/*===OmniBridgeOs/kernel/arch/x64/user/procfs.c===*/
/*
 * 最小 /proc 虚拟文件系统（第 19 步）。
 *
 * 只读；不持久化；内容按需生成。
 *
 * 人工必须审查：
 *   - /proc/self 每次 lookup 时都解析为调用进程（cur），不缓存；
 *   - read 回调只读 task_t 中已授权的字段，禁止访问内核地址；
 *   - 内容必须是纯 ASCII，无控制字符；
 *   - 大文件一次性返回，不做偏移管理（本步足够）。
 */
#include "vfs.h"
#include "task.h"
#include "sched.h"
#include "serial.h"
#include "kmalloc.h"
#include "pmm.h"
#include "user.h"

/* ---------- inode 私有数据 ---------- */
enum procfs_kind {
    PK_DIR      = 0,
    PK_FILE_FIX = 1,   /* 固定内容（const 字符串） */
    PK_FILE_GEN = 2,   /* 按任务上下文动态生成 */
};

typedef int (*procfs_gen_fn)(struct task_t *cur, char *buf,
                             uint64_t cap, uint64_t *out_len);

struct procfs_node {
    char              name[64];
    enum procfs_kind  kind;
    const char       *fixed;      /* PK_FILE_FIX 用 */
    procfs_gen_fn     gen;        /* PK_FILE_GEN 用 */
    struct procfs_node *children;
    struct procfs_node *next;
};

struct procfs_inode_priv {
    struct procfs_node *node;
};

/* ---------- 静态根树 ---------- */

/* 生成 /proc/self/status 内容 */
static int gen_self_status(struct task_t *cur, char *buf,
                           uint64_t cap, uint64_t *out_len)
{
    if (!cur) return -1;
    /* 简单格式：Name / Pid / PPid / Uid / Gid / State */
    uint64_t pos = 0;
    #define PUTS(s) do {                                   \
        const char *__s = (s);                             \
        while (*__s && pos + 1 < cap) buf[pos++] = *__s++; \
    } while (0)
    #define PUTU(u) do {                                   \
        char __t[24];                                      \
        int __n = 0;                                       \
        uint64_t __v = (uint64_t)(u);                      \
        if (__v == 0) __t[__n++] = '0';                    \
        while (__v) { __t[__n++] = (char)('0' + (__v % 10)); __v /= 10; } \
        while (__n-- > 0 && pos + 1 < cap) buf[pos++] = __t[__n]; \
    } while (0)

    PUTS("Name:\t");
    PUTS(cur->name ? cur->name : "unknown");
    PUTS("\nPid:\t");   PUTU(cur->pid);
    PUTS("\nPPid:\t");  PUTU(cur->parent_pid);
    PUTS("\nUid:\t");   PUTU(cur->security_token.uid);
    PUTS("\nGid:\t");   PUTU(cur->security_token.gid);
    PUTS("\nState:\tR\n");
    #undef PUTS
    #undef PUTU
    *out_len = pos;
    return 0;
}

static int gen_self_cmdline(struct task_t *cur, char *buf,
                            uint64_t cap, uint64_t *out_len)
{
    if (!cur) return -1;
    const char *n = cur->name ? cur->name : "unknown";
    uint64_t pos = 0;
    while (*n && pos + 2 < cap) { buf[pos++] = *n++; }
    if (pos + 1 < cap) buf[pos++] = '\0';
    *out_len = pos;
    return 0;
}

static int gen_self_maps(struct task_t *cur, char *buf,
                         uint64_t cap, uint64_t *out_len)
{
    (void)cur;
    const char *s = "00400000-00401000 r-xp 00000000 00:00 0\n";
    uint64_t pos = 0;
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    *out_len = pos;
    return 0;
}

static int gen_self_exe(struct task_t *cur, char *buf,
                        uint64_t cap, uint64_t *out_len)
{
    (void)cur;
    const char *s = "/bin/linux_hello.elf\n";
    uint64_t pos = 0;
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    *out_len = pos;
    return 0;
}

static int gen_cpuinfo(struct task_t *cur, char *buf,
                       uint64_t cap, uint64_t *out_len)
{
    (void)cur;
    const char *s =
        "processor\t: 0\n"
        "vendor_id\t: OmniBridge\n"
        "cpu family\t: 6\n"
        "model\t\t: 0\n"
        "model name\t: OmniBridge Virtual CPU\n"
        "cpu MHz\t\t: 1000.000\n"
        "cache size\t: 4096 KB\n"
        "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep\n";
    uint64_t pos = 0;
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    *out_len = pos;
    return 0;
}

static int gen_meminfo(struct task_t *cur, char *buf,
                       uint64_t cap, uint64_t *out_len)
{
    (void)cur;
    uint64_t total_pages = pmm_total_pages();
    uint64_t free_pages  = pmm_free_pages_count();
    uint64_t total_kb = total_pages * (PAGE_SIZE / 1024);
    uint64_t free_kb  = free_pages  * (PAGE_SIZE / 1024);

    uint64_t pos = 0;
    #define PUTS_(s) do { const char *__s=(s); \
        while (*__s && pos + 1 < cap) buf[pos++] = *__s++; } while (0)
    #define PUTU_(u) do { \
        char __t[24]; int __n = 0; uint64_t __v = (uint64_t)(u); \
        if (__v == 0) __t[__n++]='0'; \
        while (__v) { __t[__n++] = (char)('0' + (__v % 10)); __v /= 10; } \
        while (__n-- > 0 && pos + 1 < cap) buf[pos++] = __t[__n]; \
    } while (0)

    PUTS_("MemTotal:       "); PUTU_(total_kb); PUTS_(" kB\n");
    PUTS_("MemFree:        "); PUTU_(free_kb);  PUTS_(" kB\n");
    PUTS_("Buffers:        0 kB\n");
    PUTS_("Cached:         0 kB\n");
    PUTS_("SwapTotal:      0 kB\n");
    PUTS_("SwapFree:       0 kB\n");
    #undef PUTS_
    #undef PUTU_
    *out_len = pos;
    return 0;
}

static int gen_version(struct task_t *cur, char *buf,
                       uint64_t cap, uint64_t *out_len)
{
    (void)cur;
    const char *s =
        "Linux version 6.0.0-omnibridge (obcc@omnibridge) "
        "(clang) #1 SMP PREEMPT\n";
    uint64_t pos = 0;
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    *out_len = pos;
    return 0;
}

/* ---------- 静态节点表 ---------- */
static struct procfs_node g_node_exe = {
    "exe", PK_FILE_GEN, 0, gen_self_exe, 0, 0
};
static struct procfs_node g_node_status = {
    "status", PK_FILE_GEN, 0, gen_self_status, 0, &g_node_exe
};
static struct procfs_node g_node_cmdline = {
    "cmdline", PK_FILE_GEN, 0, gen_self_cmdline, 0, &g_node_status
};
static struct procfs_node g_node_maps = {
    "maps", PK_FILE_GEN, 0, gen_self_maps, 0, &g_node_cmdline
};
static struct procfs_node g_node_self = {
    "self", PK_DIR, 0, 0, &g_node_maps, 0
};
static struct procfs_node g_node_cpuinfo = {
    "cpuinfo", PK_FILE_GEN, 0, gen_cpuinfo, 0, &g_node_self
};
static struct procfs_node g_node_meminfo = {
    "meminfo", PK_FILE_GEN, 0, gen_meminfo, 0, &g_node_cpuinfo
};
static struct procfs_node g_node_version = {
    "version", PK_FILE_GEN, 0, gen_version, 0, &g_node_meminfo
};

/* 根节点（不参与 lookup，仅用于 readdir 起点） */
static struct procfs_node g_proc_root = {
    "", PK_DIR, 0, 0, &g_node_version, 0
};

/* ---------- ops ---------- */

static struct procfs_inode_priv *priv_of(struct vfs_inode *i)
{
    return i ? (struct procfs_inode_priv *)i->fs_data : 0;
}

/* 遍历 children 链，按 name 查找 */
static struct procfs_node *find_child(struct procfs_node *dir, const char *name)
{
    if (!dir) return 0;
    for (struct procfs_node *n = dir->children; n; n = n->next) {
        const char *a = n->name;
        const char *b = name;
        int eq = 1;
        while (*a && *b) { if (*a != *b) { eq = 0; break; } ++a; ++b; }
        if (eq && *a == '\0' && *b == '\0') return n;
    }
    return 0;
}

static int procfs_lookup(struct vfs_inode *dir, const char *name,
                         struct vfs_inode **out)
{
    *out = 0;
    if (!dir || !name) return OB_EINVAL;

    struct procfs_inode_priv *dp = priv_of(dir);
    if (!dp || dp->node->kind != PK_DIR) return OB_ENOTDIR;

    struct procfs_node *found = find_child(dp->node, name);
    if (!found) return OB_ENOENT;

    struct vfs_inode *ni = (struct vfs_inode *)kzalloc(sizeof(*ni));
    if (!ni) return OB_ENOMEM;
    struct procfs_inode_priv *np =
        (struct procfs_inode_priv *)kzalloc(sizeof(*np));
    if (!np) { kfree(ni); return OB_ENOMEM; }
    np->node = found;

    ni->mode     = (found->kind == PK_DIR)
                   ? (uint16_t)(VFS_S_IFDIR | 0555)
                   : (uint16_t)(VFS_S_IFREG | 0444);
    ni->sb       = dir->sb;
    ni->ops      = dir->ops;
    ni->fs_data  = np;
    ni->refcount = 1;

    *out = ni;
    return 0;
}

static int procfs_readdir(struct vfs_inode *dir, uint64_t index,
                          struct vfs_dirent *out)
{
    if (!dir || !out) return OB_EINVAL;
    struct procfs_inode_priv *dp = priv_of(dir);
    if (!dp || dp->node->kind != PK_DIR) return OB_ENOTDIR;

    uint64_t i = 0;
    for (struct procfs_node *n = dp->node->children; n; n = n->next) {
        if (i == index) {
            out->ino      = 0;
            out->type     = (n->kind == PK_DIR) ? VFS_FT_DIR : VFS_FT_REG;
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

/* ★ 关键：read 时按调用进程生成内容 */
static int64_t procfs_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;
    struct procfs_inode_priv *pp = priv_of(f->f_inode);
    if (!pp || pp->node->kind != PK_FILE_GEN) return OB_EINVAL;

    struct task_t *cur = task_from_thread(sched_current());

    char ktmp[2048];
    uint64_t total = 0;
    if (pp->node->gen) {
        if (pp->node->gen(cur, ktmp, sizeof(ktmp), &total) != 0) return OB_EIO;
    }

    if (f->f_pos >= total) return 0;
    uint64_t avail = total - f->f_pos;
    uint64_t take  = count < avail ? count : avail;
    uint8_t *dst = (uint8_t *)buf;
    for (uint64_t i = 0; i < take; ++i) dst[i] = (uint8_t)ktmp[f->f_pos + i];
    f->f_pos += take;
    return (int64_t)take;
}

static int64_t procfs_write(struct vfs_file *f, const void *buf, uint64_t count)
{
    (void)f; (void)buf; (void)count;
    return OB_EROFS;
}

static void procfs_evict_inode(struct vfs_inode *inode)
{
    if (!inode) return;
    if (inode->fs_data) kfree(inode->fs_data);
    kfree(inode);
}

static void procfs_destroy_sb(struct vfs_superblock *sb)
{
    if (!sb) return;
    if (sb->root) {
        if (sb->root->fs_data) kfree(sb->root->fs_data);
        kfree(sb->root);
    }
    kfree(sb);
}

static const struct vfs_operations g_procfs_ops = {
    .lookup      = procfs_lookup,
    .read        = procfs_read,
    .write       = procfs_write,
    .readdir     = procfs_readdir,
    .evict_inode = procfs_evict_inode,
    .destroy_sb  = procfs_destroy_sb,
};

void procfs_init(void)
{
    serial_printf("[PROCFS] init\n");
}

struct vfs_superblock *procfs_mount(void)
{
    struct vfs_superblock *sb =
        (struct vfs_superblock *)kzalloc(sizeof(*sb));
    if (!sb) return 0;

    struct vfs_inode *root =
        (struct vfs_inode *)kzalloc(sizeof(*root));
    if (!root) { kfree(sb); return 0; }

    struct procfs_inode_priv *rp =
        (struct procfs_inode_priv *)kzalloc(sizeof(*rp));
    if (!rp) { kfree(root); kfree(sb); return 0; }
    rp->node = &g_proc_root;

    root->ino      = 1;
    root->mode     = VFS_S_IFDIR | 0555;
    root->links    = 1;
    root->sb       = sb;
    root->ops      = &g_procfs_ops;
    root->fs_data  = rp;
    root->refcount = 1;

    sb->magic        = 0x50524F43u;   /* "PROC" */
    sb->block_size   = 4096;
    sb->ops          = &g_procfs_ops;
    sb->root         = root;

    serial_printf("[PROCFS] mounted\n");
    return sb;
}
/*===OmniBridgeOs/kernel/arch/x64/user/procfs.c 结束===*/