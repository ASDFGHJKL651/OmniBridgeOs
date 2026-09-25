/*===OmniBridgeOs/kernel/arch/x64/vfs.c===*/
#include "vfs.h"
#include "kmalloc.h"
#include "serial.h"
#include "permission.h"
#include "audit.h"

/*
 * VFS 核心实现（第 11 步）。
 *
 * API 约定（人工必须审查）：
 *   - vfs_lookup(path, out)              —— 2 参；out 为 vfs_inode **
 *                                           语义为"借用"指针，调用方不释放。
 *   - vfs_readdir(inode, index, out)     —— 3 参；第一参为 inode，
 *                                           直接转发给 ops->readdir。
 *   - vfs_rmdir(path)                    —— 与 vfs_mkdir 同构。
 *
 * 关键不变量：
 *   1) 全局挂载链表 g_mounts 只在持有 g_vfs_lock 时访问。
 *   2) vfs_lookup / vfs_open / vfs_mkdir / vfs_unlink / vfs_rmdir
 *      在解析路径的最早期执行 path_is_kernel_protected()，
 *      命中立即返回 OB_EPERM 并调用 audit_critical_access()。
 *   3) 本步不建立持久 dentry 缓存；路径解析在栈上完成，
 *      返回值直接指向 FS 持有的 vfs_inode。
 *   4) 本步仅支持绝对路径。
 */

/* ---------- 全局挂载表 ---------- */
static struct vfs_superblock *g_mounts   = 0;
static spinlock_t              g_vfs_lock = SPINLOCK_INIT;

/* ---------- 工具函数 ---------- */

static int path_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a; ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

static size_t ob_strlen(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

/* ============================================================
 * 初始化
 * ============================================================ */
void vfs_init(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);
    g_mounts = 0;
    spin_unlock_irqrestore(&g_vfs_lock, flags);

    serial_printf("[VFS] init: empty mount table\n");
}

/* ============================================================
 * 挂载 / 卸载
 * ============================================================ */

int vfs_mount(const char *path, struct vfs_superblock *sb)
{
    if (!path || !sb) return OB_EINVAL;
    if (path[0] != '/') return OB_EINVAL;
    if (!sb->root || !sb->ops) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);

    for (struct vfs_superblock *s = g_mounts; s; s = s->next) {
        if (path_eq(s->mount_path, path)) {
            spin_unlock_irqrestore(&g_vfs_lock, flags);
            serial_printf("[VFS] mount: '%s' already mounted\n", path);
            return OB_EAGAIN;
        }
    }

    size_t n = 0;
    while (path[n] && n < sizeof(sb->mount_path) - 1) {
        sb->mount_path[n] = path[n];
        ++n;
    }
    sb->mount_path[n] = '\0';

    sb->next = g_mounts;
    g_mounts = sb;

    spin_unlock_irqrestore(&g_vfs_lock, flags);

    serial_printf("[VFS] mounted '%s' (magic=0x%x block_size=%u)\n",
                  path,
                  (unsigned)sb->magic,
                  (unsigned)sb->block_size);
    return 0;
}

int vfs_umount(const char *path)
{
    if (!path) return OB_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);

    struct vfs_superblock **pp = &g_mounts;
    while (*pp && !path_eq((*pp)->mount_path, path)) {
        pp = &(*pp)->next;
    }
    if (!*pp) {
        spin_unlock_irqrestore(&g_vfs_lock, flags);
        return OB_ENOENT;
    }

    struct vfs_superblock *sb = *pp;
    *pp = sb->next;

    spin_unlock_irqrestore(&g_vfs_lock, flags);

    if (sb->ops && sb->ops->destroy_sb) {
        sb->ops->destroy_sb(sb);
    }

    serial_printf("[VFS] unmounted '%s'\n", path);
    return 0;
}

uint32_t vfs_mount_count(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);

    uint32_t n = 0;
    for (struct vfs_superblock *s = g_mounts; s; s = s->next) ++n;

    spin_unlock_irqrestore(&g_vfs_lock, flags);
    return n;
}

int vfs_is_mounted(const char *path)
{
    if (!path) return 0;

    uint64_t flags;
    spin_lock_irqsave(&g_vfs_lock, &flags);

    int found = 0;
    for (struct vfs_superblock *s = g_mounts; s; s = s->next) {
        if (path_eq(s->mount_path, path)) { found = 1; break; }
    }

    spin_unlock_irqrestore(&g_vfs_lock, flags);
    return found;
}

/* ============================================================
 * 挂载点前缀匹配
 * ============================================================ */
static struct vfs_superblock *find_mount_for(const char *path)
{
    struct vfs_superblock *best = 0;
    size_t best_len = 0;

    for (struct vfs_superblock *s = g_mounts; s; s = s->next) {
        size_t len = ob_strlen(s->mount_path);
        if (len == 0) continue;

        int match = 1;
        for (size_t i = 0; i < len; ++i) {
            if (path[i] != s->mount_path[i]) { match = 0; break; }
        }
        if (!match) continue;

        if (s->mount_path[len - 1] != '/') {
            char next = path[len];
            if (next != '\0' && next != '/') continue;
        }

        if (len >= best_len) {
            best = s;
            best_len = len;
        }
    }
    return best;
}

/* ============================================================
 * 路径解析
 * ============================================================ */

#define VFS_PATH_MAX_DEPTH 32

/* ★ 公共 API —— 返回借用 inode 指针
 *
 * 调用方**不负责**释放返回的 inode；其生命周期由具体 FS
 * （tmpfs / obfs）内部管理。本步不含 inode 引用计数。
 */
int vfs_lookup(const char *path, struct vfs_inode **out)
{
    if (!path || !out) return OB_EINVAL;
    *out = 0;

    if (path_is_kernel_protected(path)) {
        audit_critical_access(0, path, OB_ACCESS_READ);
        return OB_EPERM;
    }

    if (path[0] != '/') return OB_EINVAL;

    uint64_t irqf;
    spin_lock_irqsave(&g_vfs_lock, &irqf);
    struct vfs_superblock *sb = find_mount_for(path);
    spin_unlock_irqrestore(&g_vfs_lock, irqf);

    if (!sb) return OB_ENOENT;
    if (!sb->root) return OB_EIO;

    size_t skip = ob_strlen(sb->mount_path);
    const char *p = path + skip;

    struct vfs_inode *cur = sb->root;
    struct vfs_inode *stack[VFS_PATH_MAX_DEPTH];
    int depth = 0;

    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;

        char name[VFS_NAME_MAX];
        uint32_t nl = 0;
        while (*p && *p != '/' && nl < sizeof(name) - 1) {
            name[nl++] = *p++;
        }
        name[nl] = '\0';

        if (name[0] == '.' && name[1] == '\0') continue;

        if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
            if (depth > 0) cur = stack[--depth];
            continue;
        }

        if (!VFS_S_ISDIR(cur->mode)) return OB_ENOTDIR;
        if (!cur->ops || !cur->ops->lookup) return OB_ENOSYS;

        struct vfs_inode *next = 0;
        int rc = cur->ops->lookup(cur, name, &next);
        if (rc != 0) return rc;
        if (!next) return OB_ENOENT;

        if (depth < VFS_PATH_MAX_DEPTH) stack[depth++] = cur;
        cur = next;
    }

    *out = cur;
    return 0;
}

/* ============================================================
 * 拆分路径为父目录 + 最后一段 name
 * ============================================================ */
static int split_parent(const char *path,
                        char *parent, size_t psz,
                        char *name,   size_t nsz)
{
    if (!path || path[0] != '/') return -1;

    size_t len = ob_strlen(path);
    if (len == 0) return -1;

    size_t end = len;
    while (end > 1 && path[end - 1] == '/') --end;

    size_t slash = end;
    while (slash > 0 && path[slash - 1] != '/') --slash;

    size_t nlen = end - slash;
    if (nlen == 0 || nlen >= nsz) return -1;
    for (size_t i = 0; i < nlen; ++i) name[i] = path[slash + i];
    name[nlen] = '\0';

    size_t plen = (slash == 0) ? 1 : slash;
    while (plen > 1 && path[plen - 1] == '/') --plen;
    if (plen == 0 || plen >= psz) return -1;
    for (size_t i = 0; i < plen; ++i) parent[i] = path[i];
    parent[plen] = '\0';

    return 0;
}

/* ============================================================
 * 打开
 * ============================================================ */
int vfs_open(const char *path, uint32_t flags, struct vfs_file **out)
{
    if (!path || !out) return OB_EINVAL;
    *out = 0;

    if (path_is_kernel_protected(path)) {
        uint32_t mode = (flags & VFS_O_WRONLY) ? OB_ACCESS_WRITE
                                               : OB_ACCESS_READ;
        audit_critical_access(0, path, mode);
        return OB_EPERM;
    }

    struct vfs_inode *ino = 0;
    int want_create = (flags & VFS_O_CREAT) ? 1 : 0;

    int rc = vfs_lookup(path, &ino);

    if (rc == 0 && ino) {
        if (want_create && (flags & VFS_O_EXCL)) {
            return OB_EEXIST;
        }
    } else if (rc == OB_ENOENT && want_create) {
        char parent[VFS_PATH_MAX];
        char name[VFS_NAME_MAX];
        if (split_parent(path, parent, sizeof(parent),
                               name,   sizeof(name)) != 0) {
            return OB_EINVAL;
        }

        struct vfs_inode *dir = 0;
        int prc = vfs_lookup(parent, &dir);
        if (prc != 0 || !dir) return prc ? prc : OB_ENOENT;

        if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;
        if (!dir->ops || !dir->ops->create) return OB_EROFS;

        rc = dir->ops->create(dir, name,
                              (uint16_t)(VFS_S_IFREG | 0644), &ino);
        if (rc != 0) return rc;
    } else {
        return rc;
    }

    if (!ino) return OB_ENOENT;

    if (VFS_S_ISDIR(ino->mode) && (flags & VFS_O_WRONLY)) {
        return OB_EISDIR;
    }

    struct vfs_file *f = (struct vfs_file *)kzalloc(sizeof(*f));
    if (!f) return OB_ENOMEM;

    f->f_inode  = ino;
    f->f_flags  = flags;
    f->f_pos    = 0;
    f->refcount = 1;

    if (ino->ops && ino->ops->open) {
        rc = ino->ops->open(ino, f);
        if (rc != 0) {
            kfree(f);
            return rc;
        }
    }

    *out = f;
    return 0;
}

/* ============================================================
 * 读 / 写
 * ============================================================ */
int64_t vfs_read(struct vfs_file *f, void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;
    if (!f->f_inode->ops || !f->f_inode->ops->read) return OB_ENOSYS;

    uint64_t before = f->f_pos;
    int64_t rc = f->f_inode->ops->read(f, buf, count);
    if (rc > 0 && f->f_pos == before) {
        f->f_pos = before + (uint64_t)rc;
    }
    return rc;
}

int64_t vfs_write(struct vfs_file *f, const void *buf, uint64_t count)
{
    if (!f || !f->f_inode || !buf) return OB_EINVAL;
    if (!f->f_inode->ops || !f->f_inode->ops->write) return OB_ENOSYS;

    uint64_t before = f->f_pos;
    int64_t rc = f->f_inode->ops->write(f, buf, count);
    if (rc > 0 && f->f_pos == before) {
        f->f_pos = before + (uint64_t)rc;
    }
    return rc;
}

int vfs_close(struct vfs_file *f)
{
    if (!f) return OB_EINVAL;

    if (f->f_inode && f->f_inode->ops && f->f_inode->ops->close) {
        f->f_inode->ops->close(f);
    }

    kfree(f);
    return 0;
}

/* ============================================================
 * 目录操作
 * ============================================================ */
int vfs_mkdir(const char *path, uint16_t mode)
{
    if (!path) return OB_EINVAL;

    if (path_is_kernel_protected(path)) {
        audit_critical_access(0, path, OB_ACCESS_WRITE);
        return OB_EPERM;
    }

    char parent[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];
    if (split_parent(path, parent, sizeof(parent),
                           name,   sizeof(name)) != 0) {
        return OB_EINVAL;
    }

    struct vfs_inode *dir = 0;
    int rc = vfs_lookup(parent, &dir);
    if (rc != 0 || !dir) return rc ? rc : OB_ENOENT;

    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;
    if (!dir->ops || !dir->ops->mkdir) return OB_EROFS;

    struct vfs_inode *newdir = 0;
    return dir->ops->mkdir(dir, name,
                           (uint16_t)(VFS_S_IFDIR | (mode & 0x0FFF)),
                           &newdir);
}

int vfs_unlink(const char *path)
{
    if (!path) return OB_EINVAL;

    if (path_is_kernel_protected(path)) {
        audit_critical_access(0, path, OB_ACCESS_DELETE);
        return OB_EPERM;
    }

    char parent[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];
    if (split_parent(path, parent, sizeof(parent),
                           name,   sizeof(name)) != 0) {
        return OB_EINVAL;
    }

    struct vfs_inode *dir = 0;
    int rc = vfs_lookup(parent, &dir);
    if (rc != 0 || !dir) return rc ? rc : OB_ENOENT;

    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;
    if (!dir->ops || !dir->ops->unlink) return OB_EROFS;

    return dir->ops->unlink(dir, name);
}

int vfs_rmdir(const char *path)
{
    if (!path) return OB_EINVAL;

    if (path_is_kernel_protected(path)) {
        audit_critical_access(0, path, OB_ACCESS_DELETE);
        return OB_EPERM;
    }

    char parent[VFS_PATH_MAX];
    char name[VFS_NAME_MAX];
    if (split_parent(path, parent, sizeof(parent),
                           name,   sizeof(name)) != 0) {
        return OB_EINVAL;
    }

    struct vfs_inode *dir = 0;
    int rc = vfs_lookup(parent, &dir);
    if (rc != 0 || !dir) return rc ? rc : OB_ENOENT;

    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;
    if (!dir->ops || !dir->ops->rmdir) return OB_EROFS;

    return dir->ops->rmdir(dir, name);
}

/* ★ 3 参版本：第一参为 inode 指针 */
int vfs_readdir(struct vfs_inode *dir, uint64_t index,
                struct vfs_dirent *out)
{
    if (!dir || !out) return OB_EINVAL;
    if (!VFS_S_ISDIR(dir->mode)) return OB_ENOTDIR;
    if (!dir->ops || !dir->ops->readdir) return OB_ENOSYS;

    return dir->ops->readdir(dir, index, out);
}
/*===OmniBridgeOs/kernel/arch/x64/vfs.c 结束===*/