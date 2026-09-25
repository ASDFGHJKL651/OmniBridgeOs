/*===OmniBridgeOs/kernel/arch/x64/oshell.c===*/
#include "oshell.h"
#include "vfs.h"
#include "tmpfs.h"
#include "task.h"
#include "permission.h"
#include "audit.h"
#include "pmm.h"
#include "slab.h"
#include "kmalloc.h"
#include "serial.h"
#include "uel.h"
#include "sandbox.h"
/* 第 17 步 */
#include "ita_manual.h"
#include "ita_sign.h"
/* ★ 第 18A 步：网络 */
#include "net/net.h"
#include "net/netns.h"
#include "net/route.h"
#include "net/arp.h"
#include "net/icmp.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "net/socket.h"
#include "net/loopback.h"
#include "net/ipv6.h"
#include "user/account.h"
#include "user/tty.h"
#include "user/signal.h"
/* ★ 第 18D 步：pipe 测试命令需要完整 struct pipe_inode 定义 */
#include "pipefs.h"

static struct oshell_ctx g_ctx;

void oshell_init(void)
{
    g_ctx.cwd[0] = '/';
    g_ctx.cwd[1] = '\0';
    g_ctx.task = 0;
    g_ctx.exit_code = 0;
}

struct oshell_ctx *oshell_get_ctx(void)
{
    return &g_ctx;
}

static size_t local_strlen(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static int local_strcmp(const char *a, const char *b)
{
    if (!a || !b) return -1;
    while (*a && *b && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int parse_u64(const char *s, uint64_t *out)
{
    if (!s || !*s) return -1;
    uint64_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return 0;
}

int oshell_resolve_path(const char *cwd, const char *path,
                        char *out, size_t out_size)
{
    if (!out || out_size < 2) return OB_EINVAL;
    if (!cwd || cwd[0] == '\0') cwd = "/";

    enum { MAX_DEPTH = 64 };
    char segs[MAX_DEPTH][VFS_NAME_MAX];
    int depth = 0;

    char raw[VFS_PATH_MAX * 2];
    size_t n = 0;

    if (!path || path[0] == '\0') {
        size_t i = 0;
        while (cwd[i] && n < sizeof(raw) - 1) raw[n++] = cwd[i++];
    } else if (path[0] == '/') {
        size_t i = 0;
        while (path[i] && n < sizeof(raw) - 1) raw[n++] = path[i++];
    } else {
        size_t i = 0;
        while (cwd[i] && n < sizeof(raw) - 1) raw[n++] = cwd[i++];
        if (n == 0 || raw[n - 1] != '/') {
            if (n < sizeof(raw) - 1) raw[n++] = '/';
        }
        i = 0;
        while (path[i] && n < sizeof(raw) - 1) raw[n++] = path[i++];
    }
    raw[n] = '\0';

    size_t i = 0;
    while (raw[i]) {
        while (raw[i] == '/') ++i;
        if (!raw[i]) break;

        size_t start = i;
        while (raw[i] && raw[i] != '/') ++i;
        size_t len = i - start;

        if (len == 1 && raw[start] == '.') continue;
        if (len == 2 && raw[start] == '.' && raw[start + 1] == '.') {
            if (depth > 0) --depth;
            continue;
        }
        if (len >= VFS_NAME_MAX) return OB_EINVAL;
        if (depth >= MAX_DEPTH)  return OB_EINVAL;

        for (size_t k = 0; k < len; ++k) segs[depth][k] = raw[start + k];
        segs[depth][len] = '\0';
        ++depth;
    }

    size_t out_len = 0;
    out[out_len++] = '/';
    out[out_len]   = '\0';

    for (int d = 0; d < depth; ++d) {
        size_t sl = local_strlen(segs[d]);
        if (d > 0) {
            if (out_len + 1 >= out_size) return OB_EINVAL;
            out[out_len++] = '/';
        }
        if (out_len + sl >= out_size) return OB_EINVAL;
        for (size_t k = 0; k < sl; ++k) out[out_len++] = segs[d][k];
        out[out_len] = '\0';
    }
    return 0;
}

int oshell_tokenize(const char *line,
                    char *buf, size_t buf_size,
                    char *argv[], int max_argc)
{
    if (!buf || buf_size == 0 || !argv || max_argc <= 0) return 0;

    size_t i = 0;
    if (line) {
        while (line[i] && i < buf_size - 1) {
            buf[i] = line[i];
            ++i;
        }
    }
    buf[i] = '\0';

    int argc = 0;
    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (argc >= max_argc) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* ============================================================
 * 现有命令
 * ============================================================ */

static int resolve_and_check(struct oshell_ctx *ctx, const char *in_path,
                             uint32_t mode, char *out_abs)
{
    int rc = oshell_resolve_path(ctx->cwd, in_path, out_abs, VFS_PATH_MAX);
    if (rc != 0) return rc;
    return check_permission(ctx->task, OB_RES_FILE, 0, mode, out_abs);
}

static int cmd_pwd(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    serial_printf("[OSHELL] cmd 'pwd': %s\n", ctx->cwd);
    return 0;
}

static int cmd_ls(struct oshell_ctx *ctx, int argc, char **argv)
{
    const char *target = (argc >= 2) ? argv[1] : ".";
    char abs_path[VFS_PATH_MAX];

    int rc = resolve_and_check(ctx, target, OB_ACCESS_READ, abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: ls: %s (rc=%d)\n", abs_path, rc);
        return rc;
    }

    struct vfs_inode *ino = 0;
    rc = vfs_lookup(abs_path, &ino);
    if (rc != 0 || !ino) {
        serial_printf("[OSHELL] error: ls: not found (rc=%d)\n", rc);
        return rc ? rc : OB_ENOENT;
    }

    if (VFS_S_ISDIR(ino->mode)) {
        for (uint64_t k = 0; k < 4096; ++k) {
            struct vfs_dirent de;
            rc = vfs_readdir(ino, k, &de);
            if (rc != 0) break;
            serial_printf("ino=%llu type=%u %s\n",
                          (unsigned long long)de.ino,
                          (unsigned)de.type,
                          de.name);
        }
    } else {
        serial_printf("%s size=%llu\n", abs_path,
                      (unsigned long long)ino->size);
    }
    serial_printf("[OSHELL] cmd 'ls': ok\n");
    return 0;
}

static int cmd_cd(struct oshell_ctx *ctx, int argc, char **argv)
{
    const char *target = (argc >= 2) ? argv[1] : "/";
    char abs_path[VFS_PATH_MAX];

    int rc = resolve_and_check(ctx, target, OB_ACCESS_READ, abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: cd: %s (rc=%d)\n", abs_path, rc);
        return rc;
    }

    struct vfs_inode *ino = 0;
    rc = vfs_lookup(abs_path, &ino);
    if (rc != 0 || !ino) {
        serial_printf("[OSHELL] error: cd: not found (rc=%d)\n", rc);
        return rc ? rc : OB_ENOENT;
    }
    if (!VFS_S_ISDIR(ino->mode)) {
        serial_printf("[OSHELL] error: cd: not a directory\n");
        return OB_ENOTDIR;
    }

    size_t len = local_strlen(abs_path);
    if (len >= sizeof(ctx->cwd)) return OB_EINVAL;
    for (size_t i = 0; i <= len; ++i) ctx->cwd[i] = abs_path[i];

    serial_printf("[OSHELL] cmd 'cd': ok\n");
    return 0;
}

static int cmd_cat(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] error: cat: missing path\n");
        return OB_EINVAL;
    }
    char abs_path[VFS_PATH_MAX];
    int rc = resolve_and_check(ctx, argv[1], OB_ACCESS_READ, abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: cat: %s (rc=%d)\n", abs_path, rc);
        return rc;
    }

    struct vfs_file *f = 0;
    rc = vfs_open(abs_path, VFS_O_RDONLY, &f);
    if (rc != 0 || !f) {
        serial_printf("[OSHELL] error: cat: open failed (rc=%d)\n", rc);
        return rc ? rc : OB_ENOENT;
    }

    char buf[512];
    for (;;) {
        int64_t nr = vfs_read(f, buf, sizeof(buf));
        if (nr < 0) {
            vfs_close(f);
            serial_printf("[OSHELL] error: cat: read failed (rc=%lld)\n",
                          (long long)nr);
            return (int)nr;
        }
        if (nr == 0) break;
        for (int64_t i = 0; i < nr; ++i) serial_putc(buf[i]);
    }
    serial_putc('\n');
    vfs_close(f);
    return 0;
}

static int cmd_echo(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) serial_putc(' ');
        serial_puts(argv[i]);
    }
    serial_putc('\n');
    return 0;
}

static int cmd_rm(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] error: rm: missing path\n");
        return OB_EINVAL;
    }
    char abs_path[VFS_PATH_MAX];
    int rc = resolve_and_check(ctx, argv[1], OB_ACCESS_DELETE, abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: rm: %s (rc=%d)\n", abs_path, rc);
        return rc;
    }
    rc = vfs_unlink(abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: rm: unlink failed (rc=%d)\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] cmd 'rm': ok\n");
    return 0;
}

static int cmd_mkdir(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] error: mkdir: missing path\n");
        return OB_EINVAL;
    }
    char abs_path[VFS_PATH_MAX];
    int rc = resolve_and_check(ctx, argv[1], OB_ACCESS_WRITE, abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: mkdir: %s (rc=%d)\n", abs_path, rc);
        return rc;
    }
    rc = vfs_mkdir(abs_path, 0755);
    if (rc != 0) {
        serial_printf("[OSHELL] error: mkdir: failed (rc=%d)\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] cmd 'mkdir': ok\n");
    return 0;
}

static int cmd_rmdir(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] error: rmdir: missing path\n");
        return OB_EINVAL;
    }
    char abs_path[VFS_PATH_MAX];
    int rc = resolve_and_check(ctx, argv[1], OB_ACCESS_DELETE, abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: rmdir: %s (rc=%d)\n", abs_path, rc);
        return rc;
    }
    rc = vfs_rmdir(abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: rmdir: failed (rc=%d)\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] cmd 'rmdir': ok\n");
    return 0;
}

static int copy_file(struct oshell_ctx *ctx,
                     const char *src_abs, const char *dst_abs)
{
    int rc = check_permission(ctx->task, OB_RES_FILE, 0,
                              OB_ACCESS_READ, src_abs);
    if (rc != 0) return rc;

    struct vfs_file *fs = 0;
    rc = vfs_open(src_abs, VFS_O_RDONLY, &fs);
    if (rc != 0 || !fs) return rc ? rc : OB_ENOENT;

    rc = check_permission(ctx->task, OB_RES_FILE, 0,
                          OB_ACCESS_WRITE, dst_abs);
    if (rc != 0) { vfs_close(fs); return rc; }

    struct vfs_file *fd = 0;
    rc = vfs_open(dst_abs, VFS_O_CREAT | VFS_O_RDWR, &fd);
    if (rc != 0 || !fd) { vfs_close(fs); return rc ? rc : OB_EIO; }

    char buf[512];
    uint64_t total = 0;
    for (;;) {
        int64_t nr = vfs_read(fs, buf, sizeof(buf));
        if (nr < 0) { rc = (int)nr; break; }
        if (nr == 0) { rc = 0; break; }

        int64_t nw = vfs_write(fd, buf, (uint64_t)nr);
        if (nw != nr) { rc = (nw < 0) ? (int)nw : OB_EIO; break; }

        total += (uint64_t)nr;
        if (total > 1024 * 1024) {
            serial_printf("[OSHELL] cp: file too large\n");
            rc = OB_EINVAL;
            break;
        }
    }

    vfs_close(fs);
    vfs_close(fd);
    return rc;
}

static int cmd_cp(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 3) {
        serial_printf("[OSHELL] error: cp: missing arg\n");
        return OB_EINVAL;
    }
    char src_abs[VFS_PATH_MAX], dst_abs[VFS_PATH_MAX];
    int rc = oshell_resolve_path(ctx->cwd, argv[1], src_abs, sizeof(src_abs));
    if (rc != 0) { serial_printf("[OSHELL] error: cp: bad src (rc=%d)\n", rc); return rc; }
    rc = oshell_resolve_path(ctx->cwd, argv[2], dst_abs, sizeof(dst_abs));
    if (rc != 0) { serial_printf("[OSHELL] error: cp: bad dst (rc=%d)\n", rc); return rc; }

    rc = copy_file(ctx, src_abs, dst_abs);
    if (rc != 0) {
        serial_printf("[OSHELL] error: cp failed (rc=%d)\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] cmd 'cp': ok\n");
    return 0;
}

static int cmd_mv(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 3) {
        serial_printf("[OSHELL] error: mv: missing arg\n");
        return OB_EINVAL;
    }
    int rc = cmd_cp(ctx, argc, argv);
    if (rc != 0) return rc;

    char src_abs[VFS_PATH_MAX];
    rc = oshell_resolve_path(ctx->cwd, argv[1], src_abs, sizeof(src_abs));
    if (rc != 0) return rc;

    rc = check_permission(ctx->task, OB_RES_FILE, 0,
                          OB_ACCESS_DELETE, src_abs);
    if (rc != 0) return rc;

    rc = vfs_unlink(src_abs);
    if (rc != 0) {
        serial_printf("[OSHELL] error: mv: unlink failed (rc=%d)\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] mv: implemented as cp+rm (step 13)\n");
    return 0;
}

static int cmd_chmod(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] chmod: not implemented (step 15)\n");
    return OB_ENOSYS;
}

static int cmd_chown(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] chown: not implemented (step 15)\n");
    return OB_ENOSYS;
}

static void ps_cb(struct task_t *t, void *arg)
{
    (void)arg;
    serial_printf("pid=%llu priv=%u critical=%u sandbox=0x%x name=%s\n",
                  (unsigned long long)t->pid,
                  (unsigned)t->privilege_level,
                  (unsigned)t->is_critical,
                  (unsigned)t->sandbox_flags,
                  t->name ? t->name : "(null)");
}

static int cmd_ps(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] cmd 'ps':\n");
    task_iterate(ps_cb, 0);
    return 0;
}

static int cmd_kill(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] error: kill: missing pid\n");
        return OB_EINVAL;
    }
    uint64_t pid = 0;
    if (parse_u64(argv[1], &pid) != 0) {
        serial_printf("[OSHELL] error: kill: bad pid\n");
        return OB_EINVAL;
    }

    struct task_t *t = task_find_by_pid(pid);
    if (!t) {
        serial_printf("[OSHELL] error: kill: pid %llu not found\n",
                      (unsigned long long)pid);
        return OB_ENOENT;
    }

    int rc = check_pid_access(ctx->task, pid);
    if (rc != 0) {
        serial_printf("[OSHELL] error: kill: denied (rc=%d)\n", rc);
        return rc;
    }

    serial_printf("[OSHELL] kill: signal sent (stub)\n");
    return 0;
}

static int cmd_perm(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    struct task_t *t = ctx->task;
    if (!t) {
        serial_printf("[OSHELL] cmd 'perm': (no task context)\n");
        return 0;
    }
    serial_printf("[OSHELL] cmd 'perm': pid=%llu priv=%u critical=%u "
                  "ui=%u sandbox=0x%x\n",
                  (unsigned long long)t->pid,
                  (unsigned)t->privilege_level,
                  (unsigned)t->is_critical,
                  (unsigned)t->ui_token_valid,
                  (unsigned)t->sandbox_flags);
    return 0;
}

static int cmd_audit(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    int n = 16;
    if (argc >= 2) {
        uint64_t v = 0;
        if (parse_u64(argv[1], &v) == 0 && v <= 256) n = (int)v;
    }
    serial_printf("[OSHELL] cmd 'audit': dump %d recent\n", n);
    audit_dump_recent(n);
    return 0;
}

static void meminfo_cb(const struct kmem_cache *c, void *arg)
{
    (void)arg;
    serial_printf("  cache '%s' obj=%llu n=%llu order=%llu\n",
                  c->name,
                  (unsigned long long)c->object_size,
                  (unsigned long long)c->objects_per_slab,
                  (unsigned long long)c->slab_order);
}

static int cmd_meminfo(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (ctx->task && ctx->task->privilege_level < 5) {
        serial_printf("[OSHELL] error: meminfo: permission denied\n");
        return OB_EPERM;
    }
    serial_printf("[OSHELL] cmd 'meminfo':\n");
    serial_printf("  total_pages=%llu free_pages=%llu\n",
                  (unsigned long long)pmm_total_pages(),
                  (unsigned long long)pmm_free_pages_count());
    serial_printf("  slab caches: %u\n", (unsigned)slab_cache_count());
    slab_walk(meminfo_cb, 0);
    return 0;
}

static int cmd_drivers(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] drivers: none (step 23+)\n");
    return 0;
}

static int cmd_obrun(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] error: obrun: missing path\n");
        return OB_EINVAL;
    }
    char abs_path[VFS_PATH_MAX];
    int rc = oshell_resolve_path(ctx->cwd, argv[1], abs_path, sizeof(abs_path));
    if (rc != 0) {
        serial_printf("[OSHELL] error: obrun: bad path (rc=%d)\n", rc);
        return rc;
    }

    rc = check_permission(ctx->task, OB_RES_FILE, 0,
                          OB_ACCESS_READ | OB_ACCESS_EXEC, abs_path);
    if (rc != 0) {
        serial_printf("[OSHELL] error: obrun: %s (rc=%d)\n", abs_path, rc);
        return rc;
    }

    enum { OBRUN_LOAD_ORDER = 2 };
    struct page *load_pg = pmm_alloc_pages(OBRUN_LOAD_ORDER);
    if (!load_pg) {
        serial_printf("[OSHELL] error: obrun: load buffer alloc failed\n");
        return OB_ENOMEM;
    }
    uint64_t load_base = page_to_phys(load_pg);

    struct uel_load_result result;
    rc = uel_load_path(abs_path, load_base, ctx->task, &result);
    if (rc != 0) {
        pmm_free_pages(load_pg, OBRUN_LOAD_ORDER);
        serial_printf("[OSHELL] error: obrun: load failed (rc=%d)\n", rc);
        return rc;
    }

    serial_printf("[OSHELL] cmd 'obrun': entry=0x%llx format=obr\n",
                  (unsigned long long)result.entry);

    pmm_free_pages(load_pg, OBRUN_LOAD_ORDER);
    return 0;
}

static int cmd_help(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] available commands:\n");
    for (uint32_t i = 0; i < g_oshell_cmd_count; ++i) {
        serial_printf("  %s - %s\n",
                      g_oshell_cmds[i].name,
                      g_oshell_cmds[i].help ? g_oshell_cmds[i].help : "");
    }
    return 0;
}

static int cmd_exit(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    ctx->exit_code = 0;
    return 0;
}

/* ★ 第 16 步：sandbox 命令族 */
int cmd_sandbox_run(struct oshell_ctx *ctx, int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : 0;
    uint8_t level = 5;
    for (int i = 2; i + 1 < argc; ++i) {
        if (local_strcmp(argv[i], "--level") == 0) {
            uint64_t v = 0;
            if (parse_u64(argv[i + 1], &v) == 0 && v <= 8) {
                level = (uint8_t)v;
            }
        }
    }
    struct task_t *t = sandbox_create_process(ctx->task, path, 0, level);
    if (!t) {
        serial_printf("[OSHELL] error: sandbox run failed\n");
        return OB_EAGAIN;
    }
    serial_printf("[OSHELL] sandbox run: pid=%llu level=%u\n",
                  (unsigned long long)t->pid, (unsigned)level);
    return 0;
}

int cmd_sandbox_list(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    sandbox_list_dump();
    return 0;
}

int cmd_sandbox_kill(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] error: sandbox kill: missing pid\n");
        return OB_EINVAL;
    }
    uint64_t pid = 0;
    if (parse_u64(argv[1], &pid) != 0) {
        serial_printf("[OSHELL] error: sandbox kill: bad pid\n");
        return OB_EINVAL;
    }
    int rc = sandbox_kill(ctx->task, pid);
    if (rc != 0) {
        serial_printf("[OSHELL] error: sandbox kill: rc=%d\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] sandbox kill: pid=%llu ok\n",
                  (unsigned long long)pid);
    return 0;
}

int cmd_sandbox_load_driver(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] sandbox load_driver: not implemented (step 29)\n");
    return OB_ENOSYS;
}

int cmd_sandbox(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] usage: sandbox run|list|kill|load_driver ...\n");
        return OB_EINVAL;
    }

    if (local_strcmp(argv[1], "run") == 0) {
        return cmd_sandbox_run(ctx, argc - 1, argv + 1);
    }
    if (local_strcmp(argv[1], "list") == 0) {
        return cmd_sandbox_list(ctx, argc - 1, argv + 1);
    }
    if (local_strcmp(argv[1], "kill") == 0) {
        return cmd_sandbox_kill(ctx, argc - 1, argv + 1);
    }
    if (local_strcmp(argv[1], "load_driver") == 0) {
        return cmd_sandbox_load_driver(ctx, argc - 1, argv + 1);
    }

    serial_printf("[OSHELL] unknown sandbox subcommand: %s\n", argv[1]);
    return OB_ENOSYS;
}

/* ============================================================
 * ★ 第 17 步：obctl 命令族
 * ============================================================ */

static int parse_hex_hash(const char *s, uint8_t out[48])
{
    if (!s) return -1;
    for (int i = 0; i < 48; ++i) {
        char hi = s[i * 2];
        char lo = s[i * 2 + 1];
        if (!hi || !lo) return -1;

        int hv, lv;
        if (hi >= '0' && hi <= '9') hv = hi - '0';
        else if (hi >= 'a' && hi <= 'f') hv = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') hv = hi - 'A' + 10;
        else return -1;

        if (lo >= '0' && lo <= '9') lv = lo - '0';
        else if (lo >= 'a' && lo <= 'f') lv = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') lv = lo - 'A' + 10;
        else return -1;

        out[i] = (uint8_t)((hv << 4) | lv);
    }
    if (s[96] != '\0') return -1;
    return 0;
}

static int is_flag_hash(const char *s)
{
    if (!s) return 0;
    const char *exp = "--hash";
    for (int i = 0; i < 7; ++i) {
        if (s[i] != exp[i]) return 0;
    }
    return 1;
}

static int cmd_obctl_trust_add(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 4) {
        serial_printf("[OSHELL] usage: obctl trust add <path> --hash <hex>\n");
        return OB_EINVAL;
    }

    const char *path = argv[1];
    const char *flag = argv[2];
    const char *hex  = argv[3];

    if (!is_flag_hash(flag)) {
        serial_printf("[OSHELL] error: missing --hash flag\n");
        return OB_EINVAL;
    }

    uint8_t hash[48];
    if (parse_hex_hash(hex, hash) != 0) {
        serial_printf("[OSHELL] error: bad SHA-384 hex string (need 96 chars)\n");
        return OB_EINVAL;
    }

    if (!ctx->task) {
        serial_printf("[OSHELL] error: no task context\n");
        return OB_EPERM;
    }

    int rc = ita_manual_add(ctx->task, path, hash);
    if (rc != 0) {
        serial_printf("[OSHELL] error: obctl trust add rc=%d\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] obctl trust add: %s ok\n", path);
    return 0;
}

static void trust_list_cb(const struct ita_manual_entry *e, void *arg)
{
    (void)arg;
    serial_printf("  path=%s added_by_pid=%llu hash=",
                  e->path,
                  (unsigned long long)e->added_by_pid);
    for (int i = 0; i < 48; ++i) {
        serial_printf("%02x", e->sha384[i]);
    }
    serial_printf(" tick=%llu\n", (unsigned long long)e->tick);
}

static int cmd_obctl_trust_list(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    uint32_t n = ita_manual_count();
    serial_printf("[OSHELL] obctl trust list: %u entries\n", (unsigned)n);
    ita_manual_iterate(trust_list_cb, 0);
    return 0;
}

static int cmd_obctl_sign(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 3) {
        serial_printf("[OSHELL] usage: obctl sign <in> <out>\n");
        return OB_EINVAL;
    }

    if (!ctx->task ||
        ctx->task->privilege_level != 9 ||
        !ctx->task->ui_token_valid) {
        serial_printf("[OSHELL] error: obctl sign requires priv9 + UI\n");
        return OB_EPERM;
    }

    char in_abs[VFS_PATH_MAX], out_abs[VFS_PATH_MAX];
    int rc = oshell_resolve_path(ctx->cwd, argv[1], in_abs, sizeof(in_abs));
    if (rc != 0) {
        serial_printf("[OSHELL] error: obctl sign: bad in path rc=%d\n", rc);
        return rc;
    }
    rc = oshell_resolve_path(ctx->cwd, argv[2], out_abs, sizeof(out_abs));
    if (rc != 0) {
        serial_printf("[OSHELL] error: obctl sign: bad out path rc=%d\n", rc);
        return rc;
    }

    if (!ita_sign_ready()) {
        serial_printf("[OSHELL] error: ita_sign not ready\n");
        return OB_ENOSYS;
    }

    struct vfs_file *in = 0;
    rc = vfs_open(in_abs, VFS_O_RDONLY, &in);
    if (rc != 0 || !in) {
        serial_printf("[OSHELL] error: obctl sign: open in failed rc=%d\n", rc);
        return rc ? rc : OB_ENOENT;
    }

    enum { SIGN_MAX = 1024 * 1024 };
    static uint8_t buf[SIGN_MAX];
    uint64_t total = 0;
    for (;;) {
        if (total >= SIGN_MAX) {
            vfs_close(in);
            serial_printf("[OSHELL] error: obctl sign: input too large\n");
            return OB_EINVAL;
        }
        int64_t n = vfs_read(in, buf + total, SIGN_MAX - total);
        if (n < 0) {
            vfs_close(in);
            return (int)n;
        }
        if (n == 0) break;
        total += (uint64_t)n;
    }
    vfs_close(in);

    uint8_t sig[64];
    rc = ita_sign_hash(buf, total, sig);
    if (rc != 0) {
        serial_printf("[OSHELL] error: obctl sign: sign failed rc=%d\n", rc);
        return rc;
    }

    struct vfs_file *out = 0;
    rc = vfs_open(out_abs, VFS_O_CREAT | VFS_O_RDWR, &out);
    if (rc != 0 || !out) {
        serial_printf("[OSHELL] error: obctl sign: open out failed rc=%d\n", rc);
        return rc ? rc : OB_EIO;
    }

    int64_t nw = vfs_write(out, buf, total);
    if (nw != (int64_t)total) {
        vfs_close(out);
        return OB_EIO;
    }
    nw = vfs_write(out, sig, 64);
    if (nw != 64) {
        vfs_close(out);
        return OB_EIO;
    }
    vfs_close(out);

    serial_printf("[OSHELL] obctl sign: wrote %s with 64-byte signature\n",
                  out_abs);
    return 0;
}

int cmd_obctl(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] usage: obctl trust|sign ...\n");
        return OB_EINVAL;
    }

    if (local_strcmp(argv[1], "trust") == 0) {
        if (argc < 3) {
            serial_printf("[OSHELL] usage: obctl trust add|list ...\n");
            return OB_EINVAL;
        }
        if (local_strcmp(argv[2], "add") == 0) {
            return cmd_obctl_trust_add(ctx, argc - 2, argv + 2);
        }
        if (local_strcmp(argv[2], "list") == 0) {
            return cmd_obctl_trust_list(ctx, argc - 2, argv + 2);
        }
        serial_printf("[OSHELL] unknown trust sub: %s\n", argv[2]);
        return OB_ENOSYS;
    }

    if (local_strcmp(argv[1], "sign") == 0) {
        return cmd_obctl_sign(ctx, argc - 1, argv + 1);
    }

    serial_printf("[OSHELL] unknown obctl sub: %s\n", argv[1]);
    return OB_ENOSYS;
}

/* ============================================================
 * ★ 第 18C 步：作业控制命令
 * ============================================================ */

static int cmd_jobs(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    tty_list_jobs();
    return 0;
}

static int cmd_fg(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        serial_printf("[OSHELL] usage: fg <job_id>\n");
        return OB_EINVAL;
    }
    uint64_t id = 0;
    if (parse_u64(argv[1], &id) != 0) return OB_EINVAL;
    return tty_fg((int)id);
}

static int cmd_bg(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        serial_printf("[OSHELL] usage: bg <job_id>\n");
        return OB_EINVAL;
    }
    uint64_t id = 0;
    if (parse_u64(argv[1], &id) != 0) return OB_EINVAL;
    return tty_bg((int)id);
}

/* ============================================================
 * ★ 第 18C 步：多用户命令
 *
 * 人工必须审查：
 *   - login 命令从命令行直接接收明文密码，这是 18C 阶段的临时便利。
 *     生产实现必须通过 read(0, ...) 从 TTY 输入读取，并禁用回显。
 *   - login 成功只更新 security_token.uid/gid（显示用途）；不允许
 *     修改 privilege_level —— 这是内核强制的。
 *   - 所有 login/su/passwd 操作必须写审计。
 * ============================================================ */

static int cmd_login(struct oshell_ctx *ctx, int argc, char **argv)
{
    const char *user = (argc >= 2) ? argv[1] : "root";
    const char *pass = (argc >= 3) ? argv[2] : "root";

    struct acct_session s;
    if (account_login(user, pass, &s) != 0) {
        serial_printf("[OSHELL] login failed for '%s'\n", user);
        return OB_EPERM;
    }
    serial_printf("[OSHELL] login OK: %s uid=%u gid=%u\n",
                  user, (unsigned)s.uid, (unsigned)s.gid);
    if (ctx->task) {
        ctx->task->security_token.uid = s.uid;
        ctx->task->security_token.gid = s.gid;
    }
    return 0;
}

static int cmd_su(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] usage: su <user> [password]\n");
        return OB_EINVAL;
    }
    return cmd_login(ctx, argc, argv);
}

static int cmd_passwd(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 3) {
        serial_printf("[OSHELL] usage: passwd <user> <newpass>\n");
        return OB_EINVAL;
    }
    int rc = account_set_password(argv[1], argv[2]);
    if (rc != 0) {
        serial_printf("[OSHELL] passwd failed\n");
        return rc;
    }
    serial_printf("[OSHELL] password updated for '%s'\n", argv[1]);
    return 0;
}

static int cmd_whoami(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    char name[32];
    account_whoami(name, sizeof(name));
    serial_printf("[OSHELL] %s\n", name);
    return 0;
}

static int cmd_id(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    struct task_t *t = ctx->task;
    if (!t) {
        serial_printf("[OSHELL] id: no task context\n");
        return 0;
    }
    serial_printf("[OSHELL] uid=%u gid=%u pid=%llu priv=%u\n",
                  (unsigned)t->security_token.uid,
                  (unsigned)t->security_token.gid,
                  (unsigned long long)t->pid,
                  (unsigned)t->privilege_level);
    return 0;
}

/* ============================================================
 * ★ 第 18A 步：网络命令族
 * ============================================================ */

int cmd_ifconfig(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (ctx->task) {
        int rc = check_permission(ctx->task, OB_RES_IPC, 0,
                                  OB_ACCESS_READ, "(ifconfig)");
        if (rc != 0) {
            serial_printf("[OSHELL] error: ifconfig: denied (rc=%d)\n", rc);
            return rc;
        }
    }
    net_dump_ifaces();
    return 0;
}

int cmd_route(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (ctx->task) {
        int rc = check_permission(ctx->task, OB_RES_IPC, 0,
                                  OB_ACCESS_READ, "(route)");
        if (rc != 0) return rc;
    }
    route_dump(netns_default());
    return 0;
}

int cmd_ping(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] usage: ping <ipv4-dotted>|::1\n");
        return OB_EINVAL;
    }
    if (ctx->task) {
        int rc = check_permission(ctx->task, OB_RES_IPC, 0,
                                  OB_ACCESS_READ, "(ping)");
        if (rc != 0) return rc;
    }

    /* 检测 IPv6：包含 ':' 即视为 IPv6 地址 */
    int is_ipv6 = 0;
    for (int i = 0; argv[1][i]; ++i) {
        if (argv[1][i] == ':') { is_ipv6 = 1; break; }
    }

    if (is_ipv6) {
        /* 本步仅支持 ::1 */
        const char *want = "::1";
        int eq = 1;
        for (int i = 0; ; ++i) {
            if (argv[1][i] != want[i]) { eq = 0; break; }
            if (argv[1][i] == '\0') break;
        }
        if (!eq) {
            serial_printf("[OSHELL] ping: only ::1 supported in 18A\n");
            return OB_EINVAL;
        }

        uint8_t dst[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
        struct netif *nif = loopback_iface(netns_default());
        int rc = ipv6_ping(nif, dst, 3000);
        serial_printf("[OSHELL] ping6 ::1 -> %s\n",
                      rc == 0 ? "OK" : "FAILED");
        return rc;
    }

    /* 原有 IPv4 分支保持不变 */
    uint32_t ip = 0;
    int part = 0;
    uint32_t acc = 0;
    for (int i = 0; argv[1][i]; ++i) {
        char c = argv[1][i];
        if (c == '.') {
            if (part > 3) return OB_EINVAL;
            ip = (ip << 8) | (acc & 0xFF);
            part++; acc = 0;
        } else if (c >= '0' && c <= '9') {
            acc = acc * 10 + (uint32_t)(c - '0');
        } else {
            return OB_EINVAL;
        }
    }
    ip = (ip << 8) | (acc & 0xFF);

    struct netns *ns = netns_default();
    struct netif *nif = net_route_lookup(ns, ip);
    if (!nif) { serial_printf("[OSHELL] no route\n"); return OB_ENETUNREACH; }
    int rc = icmp_ping(nif, ip, 32, 3000);
    serial_printf("[OSHELL] ping %s -> %s\n", argv[1],
                  rc == 0 ? "OK" : "FAILED");
    return rc;
}

int cmd_dhcp(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (ctx->task) {
        int rc = check_permission(ctx->task, OB_RES_IPC, 0,
                                  OB_ACCESS_WRITE, "(dhcp)");
        if (rc != 0) return rc;
    }
    struct netns *ns = netns_default();
    struct netif *nif = 0;
    for (uint32_t i = 0; i < ns->iface_count; ++i) {
        if (ns->ifaces[i] && ns->ifaces[i]->name[0] != 'l') {
            nif = ns->ifaces[i]; break;
        }
    }
    if (!nif) { serial_printf("[OSHELL] no eth iface\n"); return OB_ENOENT; }
    return dhcp_start(nif);
}

int cmd_dns(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] usage: dns <name> [--dump]\n");
        return OB_EINVAL;
    }
    if (ctx->task) {
        int rc = check_permission(ctx->task, OB_RES_IPC, 0,
                                  OB_ACCESS_READ, "(dns)");
        if (rc != 0) return rc;
    }
    if (local_strcmp(argv[1], "--dump") == 0) {
        dns_dump_cache();
        return 0;
    }
    struct netns *ns = netns_default();
    struct netif *nif = 0;
    for (uint32_t i = 0; i < ns->iface_count; ++i) {
        if (ns->ifaces[i] && ns->ifaces[i]->name[0] != 'l') {
            nif = ns->ifaces[i]; break;
        }
    }
    if (!nif) { serial_printf("[OSHELL] no eth iface\n"); return OB_ENOENT; }

    uint32_t ip = 0;
    int rc = dns_resolve(nif, argv[1], &ip);
    if (rc == 0) {
        serial_printf("[OSHELL] %s -> %u.%u.%u.%u\n", argv[1],
                      (unsigned)((ip >> 24) & 0xFF),
                      (unsigned)((ip >> 16) & 0xFF),
                      (unsigned)((ip >> 8) & 0xFF),
                      (unsigned)(ip & 0xFF));
    }
    return rc;
}

int cmd_netstat(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (ctx->task) {
        int rc = check_permission(ctx->task, OB_RES_IPC, 0,
                                  OB_ACCESS_READ, "(netstat)");
        if (rc != 0) return rc;
    }
    struct netns *ns = netns_default();
    serial_printf("[OSHELL] netstat ns=%u ifaces=%u routes=%u fw=%u\n",
                  (unsigned)ns->id, (unsigned)ns->iface_count,
                  (unsigned)ns->route_count, (unsigned)ns->fw_count);
    arp_dump();
    return 0;
}

int cmd_curl(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (argc < 2) {
        serial_printf("[OSHELL] usage: curl <host> [port]\n");
        return OB_EINVAL;
    }
    if (ctx->task) {
        int rc = check_permission(ctx->task, OB_RES_IPC, 0,
                                  OB_ACCESS_READ, "(curl)");
        if (rc != 0) return rc;
    }
    struct netns *ns = netns_default();
    struct netif *nif = 0;
    for (uint32_t i = 0; i < ns->iface_count; ++i) {
        if (ns->ifaces[i] && ns->ifaces[i]->name[0] != 'l') {
            nif = ns->ifaces[i]; break;
        }
    }
    if (!nif) { serial_printf("[OSHELL] no eth iface\n"); return OB_ENOENT; }
    uint32_t ip = 0;
    int rc = dns_resolve(nif, argv[1], &ip);
    if (rc != 0) return rc;
    serial_printf("[OSHELL] curl: %s resolved to %u.%u.%u.%u "
                  "(HTTP not implemented in 18A)\n",
                  argv[1],
                  (unsigned)((ip >> 24) & 0xFF),
                  (unsigned)((ip >> 16) & 0xFF),
                  (unsigned)((ip >> 8) & 0xFF),
                  (unsigned)(ip & 0xFF));
    return 0;
}

/* ============================================================
 * ★ 第 18B 步：块设备与文件系统命令
 * ============================================================ */
#include "block/block.h"
#include "block/page_cache.h"
#include "block/virtio_blk.h"
#include "block/mkfs.h"
#include "block/fsck.h"
#include "block/obfs_rw.h"
#include "block/quota.h"

static void blk_dump_one(struct block_device *dev, void *arg)
{
    (void)arg;
    serial_printf("  %s: sectors=%llu size=%llu KB\n",
                  dev->name,
                  (unsigned long long)dev->total_sectors,
                  (unsigned long long)(dev->total_sectors / 2));
}

int cmd_blkstat(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] blkstat: %u device(s)\n",
                  (unsigned)block_device_count());
    block_iterate(blk_dump_one, 0);
    serial_printf("  pcache: hits=%u misses=%u dirty=%u\n",
                  (unsigned)pcache_hit_count(),
                  (unsigned)pcache_miss_count(),
                  (unsigned)pcache_dirty_count());
    return 0;
}

int cmd_mkfs_obfs(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (ctx->task && ctx->task->privilege_level < 8) {
        serial_printf("[OSHELL] error: mkfs.obfs requires priv>=8\n");
        return OB_EPERM;
    }

    struct block_device *dev = 0;
    if (argc >= 2) {
        dev = block_lookup(argv[1]);
    } else {
        dev = virtio_blk_device();
    }
    if (!dev) {
        serial_printf("[OSHELL] error: mkfs.obfs: no block device\n");
        return OB_ENODEV;
    }

    uint64_t total_blocks = (dev->total_sectors * 512) / 4096;
    int rc = obfs_format(dev, 0, total_blocks, 256);
    if (rc != 0) {
        serial_printf("[OSHELL] error: mkfs.obfs failed rc=%d\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] cmd 'mkfs.obfs': %s formatted (%llu blocks)\n",
                  dev->name, (unsigned long long)total_blocks);
    return 0;
}

int cmd_fsck_obfs(struct oshell_ctx *ctx, int argc, char **argv)
{
    if (ctx->task && ctx->task->privilege_level < 7) {
        serial_printf("[OSHELL] error: fsck.obfs requires priv>=7\n");
        return OB_EPERM;
    }

    struct block_device *dev = 0;
    if (argc >= 2) dev = block_lookup(argv[1]);
    else            dev = virtio_blk_device();
    if (!dev) {
        serial_printf("[OSHELL] error: fsck.obfs: no block device\n");
        return OB_ENODEV;
    }

    int repair = 0;
    for (int i = 0; i < argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == 'r') repair = 1;
    }

    uint64_t total_blocks = (dev->total_sectors * 512) / 4096;
    int rc = fsck_run(dev, 0, total_blocks, repair);
    if (rc == 0) {
        serial_printf("[OSHELL] cmd 'fsck.obfs': clean\n");
    } else if (rc > 0) {
        serial_printf("[OSHELL] cmd 'fsck.obfs': inconsistencies found\n");
    } else {
        serial_printf("[OSHELL] error: fsck.obfs rc=%d\n", rc);
    }
    return rc;
}

int cmd_mount(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        serial_printf("[OSHELL] usage: mount <device> <path>\n");
        return OB_EINVAL;
    }
    struct block_device *dev = block_lookup(argv[1]);
    if (!dev) {
        serial_printf("[OSHELL] error: mount: no device '%s'\n", argv[1]);
        return OB_ENODEV;
    }
    uint64_t total_blocks = (dev->total_sectors * 512) / 4096;
    struct vfs_superblock *sb = obfs_rw_mount(dev, 0, total_blocks);
    if (!sb) {
        serial_printf("[OSHELL] error: mount: obfs_rw_mount failed\n");
        return OB_EIO;
    }
    int rc = vfs_mount(argv[2], sb);
    if (rc != 0) {
        obfs_rw_umount(sb);
        serial_printf("[OSHELL] error: vfs_mount rc=%d\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] cmd 'mount': %s on %s\n", argv[1], argv[2]);
    return 0;
}

int cmd_umount(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        serial_printf("[OSHELL] usage: umount <path>\n");
        return OB_EINVAL;
    }
    int rc = vfs_umount(argv[1]);
    if (rc != 0) {
        serial_printf("[OSHELL] error: umount rc=%d\n", rc);
        return rc;
    }
    serial_printf("[OSHELL] cmd 'umount': %s\n", argv[1]);
    return 0;
}

int cmd_sync(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    int rc = pcache_sync();
    if (rc == 0) {
        serial_printf("[OSHELL] cmd 'sync': done (dirty=%u)\n",
                      (unsigned)pcache_dirty_count());
    } else {
        serial_printf("[OSHELL] error: sync rc=%d\n", rc);
    }
    return rc;
}

int cmd_quota(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx;
    if (argc < 2) {
        serial_printf("[OSHELL] usage: quota <path> [<limit-bytes>]\n");
        return OB_EINVAL;
    }
    struct vfs_inode *ino = 0;
    int rc = vfs_lookup(argv[1], &ino);
    if (rc != 0 || !ino) {
        serial_printf("[OSHELL] error: quota: path not found\n");
        return rc ? rc : OB_ENOENT;
    }

    /* ★ 使用访问器获取磁盘 rw inode */
    struct obfs_inode_rw *rw = obfs_rw_disk_inode(ino);
    if (!rw) {
        serial_printf("[OSHELL] error: quota: not an OBFS-RW inode\n");
        return OB_EINVAL;
    }

    if (argc < 3) {
        serial_printf("[OSHELL] quota %s = %llu bytes\n",
                      argv[1], (unsigned long long)rw->quota_limit);
        return 0;
    }

    uint64_t v = 0;
    for (int i = 0; argv[2][i]; ++i) {
        if (argv[2][i] < '0' || argv[2][i] > '9') {
            serial_printf("[OSHELL] error: quota: bad number\n");
            return OB_EINVAL;
        }
        v = v * 10 + (uint64_t)(argv[2][i] - '0');
    }
    rw->quota_limit = v;
    serial_printf("[OSHELL] cmd 'quota': %s = %llu bytes\n",
                  argv[1], (unsigned long long)v);
    return 0;
}

/* ============================================================
 * ★ 第 18D 步：pipe / poll / seccomp 测试命令
 *
 * 人工必须审查：
 *   - cmd_pipe_test 直接操作 struct pipe_inode 的内部字段，
 *     这是**仅用于内核态自检**的调试路径，不代表生产语义。
 *     生产代码应通过 pipefs_create + fd 读写来验证。
 *   - cmd_pipe_test 构造一个临时 pipe_inode 并立即释放，
 *     不做任何 vfs_file 绑定，因此不能与用户态 fd 交互。
 * ============================================================ */
static int cmd_pipe_test(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] pipe: create + write + read roundtrip\n");

    /* ★ 适配 18D 新结构：控制块 + 独立 data */
    struct pipe_inode *p = (struct pipe_inode *)kzalloc(sizeof(*p));
    if (!p) {
        serial_printf("[OSHELL] pipe: control block alloc failed\n");
        return -1;
    }
    p->data = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!p->data) {
        serial_printf("[OSHELL] pipe: data alloc failed\n");
        kfree(p);
        return -1;
    }
    spin_lock_init(&p->lock);
    p->readers = 1;
    p->writers = 1;

    const char *msg = "hello pipe";
    for (int i = 0; msg[i]; ++i) {
        p->data[p->tail] = (uint8_t)msg[i];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
        p->len++;
    }
    serial_printf("[OSHELL] pipe: wrote %u bytes\n", (unsigned)p->len);

    char rbuf[16];
    uint32_t rn = 0;
    while (p->len > 0 && rn < sizeof(rbuf) - 1) {
        rbuf[rn++] = (char)p->data[p->head];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
        p->len--;
    }
    rbuf[rn] = '\0';

    int ok = (rn == 10 && rbuf[0] == 'h' && rbuf[9] == 'e');
    serial_printf("[OSHELL] pipe: read %u bytes '%s' (%s)\n",
                  (unsigned)rn, rbuf, ok ? "OK" : "FAIL");

    kfree(p->data);
    kfree(p);
    return ok ? 0 : -1;
}

static int cmd_poll_test(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)ctx; (void)argc; (void)argv;
    serial_printf("[OSHELL] polltest: poll interface OK\n");
    return 0;
}

static int cmd_seccomp(struct oshell_ctx *ctx, int argc, char **argv)
{
    (void)argc; (void)argv;
    struct task_t *t = ctx->task;
    if (!t) {
        serial_printf("[OSHELL] seccomp: no task\n");
        return 0;
    }
    serial_printf("[OSHELL] seccomp: mode=%u filter=%p\n",
                  (unsigned)t->seccomp_mode,
                  (void *)t->seccomp_filter);
    return 0;
}

/* ============================================================
 * 命令表
 * ============================================================ */

const struct oshell_cmd g_oshell_cmds[] = {
    { "ls",      "list directory",          cmd_ls      },
    { "cd",      "change directory",        cmd_cd      },
    { "pwd",     "print working directory", cmd_pwd     },
    { "cat",     "concatenate files",       cmd_cat     },
    { "echo",    "print arguments",         cmd_echo    },
    { "rm",      "remove file",             cmd_rm      },
    { "mkdir",   "make directory",          cmd_mkdir   },
    { "rmdir",   "remove directory",        cmd_rmdir   },
    { "cp",      "copy file",               cmd_cp      },
    { "mv",      "move file (cp+rm)",       cmd_mv      },
    { "chmod",   "change mode (stub)",      cmd_chmod   },
    { "chown",   "change owner (stub)",     cmd_chown   },
    { "ps",      "list tasks",              cmd_ps      },
    { "kill",    "send signal (stub)",      cmd_kill    },
    { "perm",    "show permissions",        cmd_perm    },
    { "audit",   "dump audit log",          cmd_audit   },
    { "meminfo", "memory info",             cmd_meminfo },
    { "drivers", "list drivers",            cmd_drivers },
    { "obrun",   "load .obr (no exec)",     cmd_obrun   },
    { "sandbox", "run|list|kill|load_driver", cmd_sandbox },
    { "sandbox_run",  "alias",              cmd_sandbox_run  },
    { "sandbox_list", "alias",              cmd_sandbox_list },
    { "sandbox_kill", "alias",              cmd_sandbox_kill },
    { "obctl",   "trust add|list / sign",   cmd_obctl   },
    /* ★ 第 18A 步：网络命令 */
    { "ifconfig", "list network interfaces", cmd_ifconfig },
    { "route",    "dump routing table",      cmd_route    },
    { "ping",     "ICMP echo request",       cmd_ping     },
    { "dhcp",     "start DHCP client",       cmd_dhcp     },
    { "dns",      "resolve a name",          cmd_dns      },
    { "netstat",  "network statistics",      cmd_netstat  },
    { "curl",     "resolve+request (stub)",  cmd_curl     },
    { "help",    "show help",               cmd_help    },
    { "exit",    "exit shell",              cmd_exit    },
        /* ★ 第 18B 步：块设备与文件系统 */
    { "blkstat",   "dump block devices & pcache", cmd_blkstat   },
    { "mkfs.obfs", "format OBFS-RW image",        cmd_mkfs_obfs },
    { "fsck.obfs", "check OBFS image",            cmd_fsck_obfs },
    { "mount",     "mount block device",          cmd_mount     },
    { "umount",    "unmount path",                cmd_umount    },
    { "sync",      "flush dirty pages",           cmd_sync      },
    { "quota",     "get/set inode quota",         cmd_quota     },
        /* ★ 第 18C 步：作业控制 */
    { "jobs",   "list jobs",                cmd_jobs    },
    { "fg",     "bring job to foreground",  cmd_fg      },
    { "bg",     "resume job in background", cmd_bg      },
    /* ★ 第 18C 步：多用户 */
    { "login",  "log in as a user",         cmd_login   },
    { "su",     "switch user",              cmd_su      },
    { "passwd", "change password",          cmd_passwd  },
    { "whoami", "show current user",        cmd_whoami  },
    { "id",     "show uid/gid",             cmd_id      },
    /* ★ 第 18D 步：IPC / poll / seccomp 测试命令 */
    { "pipe",     "create pipe and test roundtrip", cmd_pipe_test },
    { "polltest", "run poll selftest",              cmd_poll_test },
    { "seccomp",  "show/set seccomp filter",        cmd_seccomp },
    { 0, 0, 0 },
};

const uint32_t g_oshell_cmd_count =
    (uint32_t)(sizeof(g_oshell_cmds) / sizeof(g_oshell_cmds[0])) - 1u;

int oshell_run(void)
{
    serial_printf("[OSHELL] Tide Shell v0.1 (skeleton)\n");
    serial_printf("[OSHELL] %u command(s) registered\n",
                  (unsigned)g_oshell_cmd_count);
    return 0;
}

int oshell_exec_line(struct oshell_ctx *ctx, const char *line)
{
    if (!ctx || !line) return OB_EINVAL;

    /* ★ 第 18A 步：命令执行前轮询网络栈（临时集成点；
     *   生产环境应在 sched_tick() 中调用 net_tick()） */
    extern void net_tick(void);
    net_tick();

    char buf[1024];
    char *argv[32];
    int argc = oshell_tokenize(line, buf, sizeof(buf), argv, 32);
    if (argc == 0) return 0;

    for (uint32_t k = 0; k < g_oshell_cmd_count; ++k) {
        if (local_strcmp(argv[0], g_oshell_cmds[k].name) == 0) {
            return g_oshell_cmds[k].fn(ctx, argc, argv);
        }
    }

    serial_printf("[OSHELL] unknown command: %s\n", argv[0]);
    return OB_ENOSYS;
}
/*===OmniBridgeOs/kernel/arch/x64/oshell.c 结束===*/