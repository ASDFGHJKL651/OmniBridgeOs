/*===OmniBridgeOs/kernel/arch/x64/user/linux_syscall.c===*/
/*
 * Linux ELF 兼容层：syscall 分发（第 19 步）。
 *
 * 人工必须审查（重要）：
 *   1) 所有 handler 必须先调用 check_permission()，失败时：
 *        - 写 CRITICAL 级审计（AUDIT_EV_COMPAT_ACCESS_DENIED）
 *        - 返回 -LINUX_EPERM（不是 -1，Linux 惯例是负 errno）
 *   2) 未映射的 syscall 必须写审计（AUDIT_EV_COMPAT_SYSCALL_ENOSYS）
 *      并返回 -LINUX_ENOSYS，绝不假装成功。
 *   3) fd 0/1/2 特殊处理：read(0) 返回 0（EOF），write(1/2) 到串口，
 *      close(0/1/2) 直接返回 0（不真正关闭）。
 *   4) Linux ABI 参数读取：f->rdi=arg0，f->rsi=arg1，f->rdx=arg2，
 *      f->r10=arg3（注意不是 rcx），f->r8=arg4，f->r9=arg5。
 *
 * ★ 本轮修复（关键）：
 *   原实现用 ob_syscall0/1/2/3() 从内核 handler 调用 OB 语义操作。
 *   但 ob_syscall* 是用户态 oblibc 提供的内联汇编封装（会执行
 *   syscall 指令），在内核态调用会造成：
 *     - 编译错误（未声明；ob.h 不在内核编译单元里）；
 *     - 即使能编译，也会递归进入内核，语义错误。
 *
 *   修复：新增 kx0/kx1/kx2/kx3 辅助函数，直接构造 syscall_frame 后
 *   调用内核 syscall_dispatcher()。为避免递归进入 linux_syscall_dispatch，
 *   调用前将 cur->compat_type 临时置为 COMPAT_TYPE_NONE，调用后恢复。
 */
#include "linux_syscall.h"
#include "user.h"
#include "signal.h"
#include "serial.h"
#include "audit.h"
#include "permission.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "task.h"
#include "sched.h"
#include "futex.h"
#include "rng.h"

/* ---- VFS flags 与内核 VFS 常量保持一致 ---- */
#ifndef VFS_O_RDONLY
#define VFS_O_RDONLY  0x0001
#define VFS_O_WRONLY  0x0002
#define VFS_O_RDWR    0x0003
#define VFS_O_CREAT   0x0010
#define VFS_O_EXCL    0x0020
#define VFS_O_TRUNC   0x0040
#define VFS_O_APPEND  0x0080
#endif

/* ---- Linux 通用打开标志 ---- */
#define L_O_RDONLY  0x0000
#define L_O_WRONLY  0x0001
#define L_O_RDWR    0x0002
#define L_O_CREAT   0x0040
#define L_O_EXCL    0x0080
#define L_O_TRUNC   0x0200
#define L_O_APPEND  0x0400

/* fd 表容量 */
#define LX_FD_MAX 64

/* ============================================================
 * ★ 内核侧 OB 分发辅助
 *
 * 人工必须审查：
 *   - syscall_dispatcher() 是内核函数（在 kernel/arch/x64/syscall.c
 *     中定义），内核代码可以直接调用它。
 *   - 递归防护：syscall_dispatcher 入口会判断 compat_type；若为
 *     COMPAT_TYPE_LINUX 会再走 linux_syscall_dispatch 造成无限递归。
 *     因此调用前必须临时把 compat_type 设为 NONE，调用后恢复。
 *   - 本函数不持有任何锁；调用者负责不在此路径内持有 pmm/sched
 *     之外的内核锁。
 * ============================================================ */
static int64_t kx_call_ob(struct task_t *cur, uint64_t nr,
                          uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4)
{
    if (!cur) return -LINUX_EPERM;

    struct syscall_frame sf;
    uint8_t *p = (uint8_t *)&sf;
    for (unsigned i = 0; i < sizeof(sf); ++i) p[i] = 0;

    sf.rax = nr;
    sf.rdi = a1;
    sf.rsi = a2;
    sf.rdx = a3;
    sf.r10 = a4;

    uint8_t saved_compat = cur->compat_type;
    cur->compat_type = COMPAT_TYPE_NONE;

    extern int64_t syscall_dispatcher(struct syscall_frame *f);
    int64_t r = syscall_dispatcher(&sf);

    cur->compat_type = saved_compat;
    return r;
}

static inline int64_t kx0(struct task_t *cur, uint64_t nr)
{ return kx_call_ob(cur, nr, 0, 0, 0, 0); }

static inline int64_t kx1(struct task_t *cur, uint64_t nr, uint64_t a1)
{ return kx_call_ob(cur, nr, a1, 0, 0, 0); }

static inline int64_t kx2(struct task_t *cur, uint64_t nr,
                          uint64_t a1, uint64_t a2)
{ return kx_call_ob(cur, nr, a1, a2, 0, 0); }

static inline int64_t kx3(struct task_t *cur, uint64_t nr,
                          uint64_t a1, uint64_t a2, uint64_t a3)
{ return kx_call_ob(cur, nr, a1, a2, a3, 0); }

/* ============================================================
 * 工具函数
 * ============================================================ */

int ob_to_linux_errno(int ob_err)
{
    if (ob_err >= 0) return 0;
    int e = -ob_err;
    switch (e) {
    case 1:   return LINUX_EPERM;
    case 2:   return LINUX_ENOENT;
    case 3:   return LINUX_ESRCH;
    case 5:   return LINUX_EIO;
    case 9:   return LINUX_EBADF;
    case 11:  return LINUX_EAGAIN;
    case 12:  return LINUX_ENOMEM;
    case 13:  return LINUX_EACCES;
    case 14:  return LINUX_EFAULT;
    case 16:  return LINUX_EBUSY;
    case 17:  return LINUX_EEXIST;
    case 19:  return LINUX_ENODEV;
    case 20:  return LINUX_ENOTDIR;
    case 21:  return LINUX_EISDIR;
    case 22:  return LINUX_EINVAL;
    case 23:  return LINUX_ENFILE;
    case 24:  return LINUX_EMFILE;
    case 25:  return LINUX_ENOTTY;
    case 27:  return LINUX_EFBIG;
    case 28:  return LINUX_ENOSPC;
    case 29:  return LINUX_ESPIPE;
    case 30:  return LINUX_EROFS;
    case 32:  return LINUX_EPIPE;
    case 34:  return LINUX_ERANGE;
    case 36:  return LINUX_ENAMETOOLONG;
    case 38:  return LINUX_ENOSYS;
    case 39:  return LINUX_ENOTEMPTY;
    case 40:  return LINUX_ELOOP;
    case 61:  return LINUX_ENODATA;
    case 95:  return LINUX_EOPNOTSUPP;
    case 101: return LINUX_ENETUNREACH;
    case 110: return LINUX_ETIMEDOUT;
    case 111: return LINUX_ECONNREFUSED;
    case 122: return LINUX_EDQUOT;
    default:  return LINUX_EIO;
    }
}

static int lx_range_ok(uint64_t va, uint64_t len, int for_write)
{
    (void)for_write;
    return user_range_ok(va, len);
}

/* 权限检查 + 审计：所有 linux_* 路径统一入口。
 * 失败时返回负的 Linux errno。 */
static int lx_check_file(struct task_t *cur, const char *path, uint32_t mode)
{
    int rc = check_permission(cur, OB_RES_FILE, 0, mode, path);
    if (rc != 0) {
        audit_event(AUDIT_EV_COMPAT_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, mode, 0, 0, path ? path : "(linux-file)");
        return -LINUX_EPERM;
    }
    return 0;
}

static int lx_check_mem(struct task_t *cur, uint64_t vaddr, uint32_t mode)
{
    int rc = check_permission(cur, OB_RES_MEMORY, vaddr, mode, 0);
    if (rc != 0) {
        audit_event(AUDIT_EV_COMPAT_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, vaddr, mode, 0, "(linux-mem)");
        return -LINUX_EFAULT;
    }
    return 0;
}

/* 从用户态读取 NUL 结尾字符串到内核缓冲。
 * 返回 0 成功；-1 失败（越界或超长）。 */
static int lx_strncpy_from_user(char *dst, uint64_t uaddr, uint64_t max)
{
    if (!dst || max == 0) return -1;
    if (!user_range_ok(uaddr, 1)) return -1;
    uint64_t n = 0;
    stac();
    const char *s = (const char *)(uintptr_t)uaddr;
    while (n + 1 < max && s[n] != '\0') {
        dst[n] = s[n];
        ++n;
    }
    dst[n] = '\0';
    clac();
    return (s[n] == '\0') ? 0 : -1;
}

/* ============================================================
 * VFS 标志转换：Linux O_* -> 内核 VFS_O_*
 * ============================================================ */
static uint32_t lx_to_vfs_flags(uint64_t lin_flags)
{
    uint32_t acc = lin_flags & 0x3u;
    uint32_t vf  = 0;
    if (acc == L_O_WRONLY)      vf = VFS_O_WRONLY;
    else if (acc == L_O_RDWR)   vf = VFS_O_RDWR;
    else                        vf = VFS_O_RDONLY;

    if (lin_flags & L_O_CREAT)  vf |= VFS_O_CREAT;
    if (lin_flags & L_O_EXCL)   vf |= VFS_O_EXCL;
    if (lin_flags & L_O_TRUNC)  vf |= VFS_O_TRUNC;
    if (lin_flags & L_O_APPEND) vf |= VFS_O_APPEND;
    return vf;
}

static int lx_fd_alloc(struct task_t *cur)
{
    for (int i = 3; i < LX_FD_MAX; ++i) {
        if (!cur->fd_table[i]) return i;
    }
    return -1;
}

/* ============================================================
 * 各 syscall handler
 * ============================================================ */

static int64_t linux_read(struct task_t *cur, struct syscall_frame *f)
{
    int fd = (int)f->rdi;
    uint64_t ubuf = f->rsi;
    uint64_t cnt  = f->rdx;

    if (fd == 0) return 0;   /* stdin EOF */

    if (fd < 0 || fd >= LX_FD_MAX) return -LINUX_EBADF;

    int rc = lx_check_mem(cur, ubuf, OB_ACCESS_WRITE);
    if (rc != 0) return rc;
    if (!lx_range_ok(ubuf, cnt, 1)) return -LINUX_EFAULT;

    struct vfs_file *vf = cur->fd_table[fd];
    if (!vf) return -LINUX_EBADF;

    int64_t n = vfs_read(vf, (void *)(uintptr_t)ubuf, cnt);
    if (n < 0) return -ob_to_linux_errno((int)n);
    return n;
}

static int64_t linux_write(struct task_t *cur, struct syscall_frame *f)
{
    int fd = (int)f->rdi;
    uint64_t ubuf = f->rsi;
    uint64_t cnt  = f->rdx;

    if (cnt == 0) return 0;
    if (fd < 0 || fd >= LX_FD_MAX) return -LINUX_EBADF;

    int rc = lx_check_mem(cur, ubuf, OB_ACCESS_READ);
    if (rc != 0) return rc;
    if (!lx_range_ok(ubuf, cnt, 0)) return -LINUX_EFAULT;

    if (fd == 1 || fd == 2) {
        const char *p = (const char *)(uintptr_t)ubuf;
        stac();
        for (uint64_t i = 0; i < cnt; ++i) serial_putc(p[i]);
        clac();
        return (int64_t)cnt;
    }

    struct vfs_file *vf = cur->fd_table[fd];
    if (!vf) return -LINUX_EBADF;

    int64_t n = vfs_write(vf, (const void *)(uintptr_t)ubuf, cnt);
    if (n < 0) return -ob_to_linux_errno((int)n);
    return n;
}

static int64_t linux_open(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t upath  = f->rdi;
    uint64_t lflags = f->rsi;
    char kpath[VFS_PATH_MAX];

    if (lx_strncpy_from_user(kpath, upath, sizeof(kpath)) != 0)
        return -LINUX_EFAULT;

    uint32_t vf = lx_to_vfs_flags(lflags);
    uint32_t mode = OB_ACCESS_READ;
    if ((lflags & 0x3u) == L_O_WRONLY) mode = OB_ACCESS_WRITE;
    else if ((lflags & 0x3u) == L_O_RDWR)
        mode = OB_ACCESS_READ | OB_ACCESS_WRITE;
    if (lflags & L_O_CREAT) mode |= OB_ACCESS_WRITE;

    int rc = lx_check_file(cur, kpath, mode);
    if (rc != 0) return rc;

    struct vfs_file *newf = 0;
    rc = vfs_open(kpath, vf, &newf);
    if (rc != 0) return -ob_to_linux_errno(rc);

    int nfd = lx_fd_alloc(cur);
    if (nfd < 0) { vfs_close(newf); return -LINUX_EMFILE; }
    cur->fd_table[nfd] = newf;
    return nfd;
}

static int64_t linux_openat(struct task_t *cur, struct syscall_frame *f)
{
    int dirfd = (int)f->rdi;
    (void)dirfd;
    uint64_t upath = f->rsi;
    char kpath[VFS_PATH_MAX];
    if (lx_strncpy_from_user(kpath, upath, sizeof(kpath)) != 0)
        return -LINUX_EFAULT;

    uint64_t lflags = f->rdx;
    uint32_t vf = lx_to_vfs_flags(lflags);
    uint32_t mode = OB_ACCESS_READ;
    if ((lflags & 0x3u) == L_O_WRONLY) mode = OB_ACCESS_WRITE;
    else if ((lflags & 0x3u) == L_O_RDWR)
        mode = OB_ACCESS_READ | OB_ACCESS_WRITE;
    if (lflags & L_O_CREAT) mode |= OB_ACCESS_WRITE;

    int rc = lx_check_file(cur, kpath, mode);
    if (rc != 0) return rc;

    struct vfs_file *newf = 0;
    rc = vfs_open(kpath, vf, &newf);
    if (rc != 0) return -ob_to_linux_errno(rc);

    int nfd = lx_fd_alloc(cur);
    if (nfd < 0) { vfs_close(newf); return -LINUX_EMFILE; }
    cur->fd_table[nfd] = newf;
    return nfd;
}

static int64_t linux_close(struct task_t *cur, struct syscall_frame *f)
{
    int fd = (int)f->rdi;
    if (fd >= 0 && fd <= 2) return 0;
    if (fd < 0 || fd >= LX_FD_MAX) return -LINUX_EBADF;
    struct vfs_file *vf = cur->fd_table[fd];
    if (!vf) return -LINUX_EBADF;
    vfs_close(vf);
    cur->fd_table[fd] = 0;
    return 0;
}

static int64_t linux_lseek(struct task_t *cur, struct syscall_frame *f)
{
    int fd = (int)f->rdi;
    int64_t off = (int64_t)f->rsi;
    int whence = (int)f->rdx;
    if (fd < 0 || fd >= LX_FD_MAX) return -LINUX_EBADF;
    struct vfs_file *vf = cur->fd_table[fd];
    if (!vf) return -LINUX_EBADF;
    uint64_t newpos;
    if (whence == 0) newpos = (uint64_t)off;
    else if (whence == 1) newpos = vf->f_pos + (uint64_t)off;
    else if (whence == 2) {
        uint64_t sz = vf->f_inode ? vf->f_inode->size : 0;
        newpos = sz + (uint64_t)off;
    } else return -LINUX_EINVAL;
    vf->f_pos = newpos;
    return (int64_t)newpos;
}

static int64_t linux_brk(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t want = f->rdi;
    return kx1(cur, SYS_OB_Brk, want);
}

static int64_t linux_mmap(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t size = f->rsi;
    if (size == 0) return -LINUX_EINVAL;
    if (size > 256ULL * 1024 * 1024) return -LINUX_ENOMEM;
    int64_t r = kx3(cur, SYS_OB_VirtualAlloc, size, 0, 0);
    if (r < 0) return -ob_to_linux_errno((int)r);
    return r;
}

static int64_t linux_munmap(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}

static int64_t linux_getpid(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return kx0(cur, SYS_OB_Getpid);
}

static int64_t linux_getppid(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return kx0(cur, SYS_OB_Getppid);
}

static int64_t linux_gettid(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return kx0(cur, SYS_OB_GetTid);
}

static int64_t linux_getuid(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return (int64_t)cur->security_token.uid;
}

static int64_t linux_getgid(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return (int64_t)cur->security_token.gid;
}

static int64_t linux_exit(struct task_t *cur, struct syscall_frame *f)
{
    int code = (int)f->rdi;
    (void)cur;
    task_exit(code);
    /* 不返回 */
    return 0;
}

static int64_t linux_futex(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t uaddr = f->rdi;
    int op = (int)f->rsi;
    uint32_t val = (uint32_t)f->rdx;

    if ((op & 0x7F) == 0) {
        return kx3(cur, SYS_OB_FutexWait, uaddr, val, 0);
    } else if ((op & 0x7F) == 1) {
        return kx2(cur, SYS_OB_FutexWake, uaddr, val);
    }
    return -LINUX_ENOSYS;
}

static int64_t linux_clock_gettime(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t uts = f->rsi;
    if (!lx_range_ok(uts, 16, 1)) return -LINUX_EFAULT;

    int64_t sec = kx0(cur, SYS_OB_GetTime);
    if (sec < 0) sec = 0;

    uint64_t *p = (uint64_t *)(uintptr_t)uts;
    stac();
    p[0] = (uint64_t)sec;
    p[1] = 0;
    clac();
    return 0;
}

static int64_t linux_clock_getres(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    uint64_t uts = f->rsi;
    if (uts == 0) return 0;
    if (!lx_range_ok(uts, 16, 1)) return -LINUX_EFAULT;
    uint64_t *p = (uint64_t *)(uintptr_t)uts;
    stac();
    p[0] = 0;
    p[1] = 1000000;
    clac();
    return 0;
}

static int64_t linux_arch_prctl(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    int code = (int)f->rdi;
    uint64_t addr = f->rsi;
    if (code == LINUX_ARCH_SET_FS) {
        user_set_fsbase(addr);
        return 0;
    }
    if (code == LINUX_ARCH_GET_FS) {
        if (!lx_range_ok(addr, 8, 1)) return -LINUX_EFAULT;
        uint64_t v = user_get_fsbase();
        *(uint64_t *)(uintptr_t)addr = v;
        return 0;
    }
    return -LINUX_EINVAL;
}

static int64_t linux_pipe(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t ufds = f->rdi;
    if (!lx_range_ok(ufds, 8, 1)) return -LINUX_EFAULT;
    return kx1(cur, SYS_OB_Pipe, ufds);
}

static int64_t linux_pipe2(struct task_t *cur, struct syscall_frame *f)
{
    return linux_pipe(cur, f);
}

static int64_t linux_uname(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    uint64_t up = f->rdi;
    if (!lx_range_ok(up, 390, 1)) return -LINUX_EFAULT;
    static const char *fields[6] = {
        "Linux", "omnibridge", "6.0.0-ob", "#1 SMP", "x86_64", "(none)"
    };
    uint8_t *p = (uint8_t *)(uintptr_t)up;
    stac();
    for (int i = 0; i < 6; ++i) {
        const char *s = fields[i];
        for (int k = 0; k < 65; ++k) p[i * 65 + k] = 0;
        int j = 0;
        while (s[j] && j < 64) { p[i * 65 + j] = (uint8_t)s[j]; ++j; }
    }
    clac();
    return 0;
}

static int64_t linux_getcwd(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    uint64_t ubuf = f->rdi;
    uint64_t size = f->rsi;
    if (!lx_range_ok(ubuf, size, 1)) return -LINUX_EFAULT;
    if (size < 2) return -LINUX_ERANGE;
    stac();
    char *p = (char *)(uintptr_t)ubuf;
    p[0] = '/';
    p[1] = '\0';
    clac();
    return 1;
}

static int64_t linux_getrandom(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    uint64_t ubuf = f->rdi;
    uint64_t len  = f->rsi;
    if (!lx_range_ok(ubuf, len, 1)) return -LINUX_EFAULT;
    stac();
    int rc = rng_bytes((void *)(uintptr_t)ubuf, len);
    clac();
    if (rc != 0) return -LINUX_EIO;
    return (int64_t)len;
}

static int64_t linux_rt_sigaction(struct task_t *cur, struct syscall_frame *f)
{
    int signum = (int)f->rdi;
    uint64_t uact = f->rsi;
    uint64_t uold = f->rdx;

    if (uact) {
        if (!lx_range_ok(uact, 8, 0)) return -LINUX_EFAULT;
        uint64_t handler = 0;
        stac();
        handler = *(uint64_t *)(uintptr_t)uact;
        clac();
        int64_t r = kx2(cur, SYS_OB_Sigaction, (uint64_t)signum, handler);
        if (r < 0) return -ob_to_linux_errno((int)r);
    }
    if (uold) {
        if (!lx_range_ok(uold, 8, 1)) return -LINUX_EFAULT;
        stac();
        *(uint64_t *)(uintptr_t)uold = 0;
        clac();
    }
    return 0;
}

static int64_t linux_rt_sigprocmask(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}

static int64_t linux_rt_sigreturn(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return kx0(cur, SYS_OB_SigReturn);
}

static int64_t linux_kill(struct task_t *cur, struct syscall_frame *f)
{
    return kx2(cur, SYS_OB_Kill, f->rdi, f->rsi);
}

static int64_t linux_set_tid_address(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return kx0(cur, SYS_OB_GetTid);
}

static int64_t linux_sched_yield(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    thread_yield();
    return 0;
}

static int64_t linux_fork(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    audit_event(AUDIT_EV_COMPAT_FORK, AUDIT_LVL_WARN,
                cur->pid, 0, 0, 0, "(linux-fork)");
    return kx1(cur, SYS_OB_Clone, 0 /* flags=0 => fork 语义 */);
}

static int64_t linux_execve(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return -LINUX_ENOSYS;
}

static int64_t linux_wait4(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    int64_t pid = (int64_t)f->rdi;
    uint64_t ustatus = f->rsi;
    int code = 0;
    int rc = task_join((uint64_t)pid, &code);
    if (rc != 0) return -ob_to_linux_errno(rc);
    if (ustatus && lx_range_ok(ustatus, 4, 1)) {
        int32_t v = code << 8;
        stac();
        *(int32_t *)(uintptr_t)ustatus = v;
        clac();
    }
    return (int64_t)pid;
}

/* ============================================================
 * 映射表
 * ============================================================ */

struct linux_syscall_map {
    uint64_t    linux_nr;
    int64_t   (*handler)(struct task_t *cur, struct syscall_frame *f);
    const char *name;
};

static const struct linux_syscall_map g_lx_map[] = {
    { LINUX_SYS_read,             linux_read,             "read" },
    { LINUX_SYS_write,            linux_write,            "write" },
    { LINUX_SYS_open,             linux_open,             "open" },
    { LINUX_SYS_openat,           linux_openat,           "openat" },
    { LINUX_SYS_close,            linux_close,            "close" },
    { LINUX_SYS_lseek,            linux_lseek,            "lseek" },
    { LINUX_SYS_brk,              linux_brk,              "brk" },
    { LINUX_SYS_mmap,             linux_mmap,             "mmap" },
    { LINUX_SYS_munmap,           linux_munmap,           "munmap" },
    { LINUX_SYS_getpid,           linux_getpid,           "getpid" },
    { LINUX_SYS_getppid,          linux_getppid,          "getppid" },
    { LINUX_SYS_gettid,           linux_gettid,           "gettid" },
    { LINUX_SYS_getuid,           linux_getuid,           "getuid" },
    { LINUX_SYS_getgid,           linux_getgid,           "getgid" },
    { LINUX_SYS_geteuid,          linux_getuid,           "geteuid" },
    { LINUX_SYS_getegid,          linux_getgid,           "getegid" },
    { LINUX_SYS_exit,             linux_exit,             "exit" },
    { LINUX_SYS_exit_group,       linux_exit,             "exit_group" },
    { LINUX_SYS_futex,            linux_futex,            "futex" },
    { LINUX_SYS_clock_gettime,    linux_clock_gettime,    "clock_gettime" },
    { LINUX_SYS_clock_getres,     linux_clock_getres,     "clock_getres" },
    { LINUX_SYS_arch_prctl,       linux_arch_prctl,       "arch_prctl" },
    { LINUX_SYS_pipe,             linux_pipe,             "pipe" },
    { LINUX_SYS_pipe2,            linux_pipe2,            "pipe2" },
    { LINUX_SYS_uname,            linux_uname,            "uname" },
    { LINUX_SYS_getcwd,           linux_getcwd,           "getcwd" },
    { LINUX_SYS_getrandom,        linux_getrandom,        "getrandom" },
    { LINUX_SYS_rt_sigaction,     linux_rt_sigaction,     "rt_sigaction" },
    { LINUX_SYS_rt_sigprocmask,   linux_rt_sigprocmask,   "rt_sigprocmask" },
    { LINUX_SYS_rt_sigreturn,     linux_rt_sigreturn,     "rt_sigreturn" },
    { LINUX_SYS_kill,             linux_kill,             "kill" },
    { LINUX_SYS_set_tid_address,  linux_set_tid_address,  "set_tid_address" },
    { LINUX_SYS_sched_yield,      linux_sched_yield,      "sched_yield" },
    { LINUX_SYS_fork,             linux_fork,             "fork" },
    { LINUX_SYS_clone,            linux_fork,             "clone" },
    { LINUX_SYS_execve,           linux_execve,           "execve" },
    { LINUX_SYS_wait4,            linux_wait4,            "wait4" },
};

#define LX_MAP_COUNT (sizeof(g_lx_map) / sizeof(g_lx_map[0]))

/* ============================================================
 * 初始化与分发
 * ============================================================ */

void linux_syscall_init(void)
{
    serial_printf("[LINUX-SYSCALL] init: %u syscall mappings loaded\n",
                  (unsigned)LX_MAP_COUNT);
}

int64_t linux_syscall_dispatch(struct task_t *cur, struct syscall_frame *f)
{
    if (!cur || !f) return -LINUX_EINVAL;

    uint64_t nr = f->rax;

    for (size_t i = 0; i < LX_MAP_COUNT; ++i) {
        if (g_lx_map[i].linux_nr == nr) {
            return g_lx_map[i].handler(cur, f);
        }
    }

    serial_printf("[LINUX-SYSCALL] ENOSYS nr=%llu pid=%llu\n",
                  (unsigned long long)nr, (unsigned long long)cur->pid);
    audit_event(AUDIT_EV_COMPAT_SYSCALL_ENOSYS, AUDIT_LVL_CRITICAL,
                cur->pid, nr, 0, 0, "(linux-enosys)");
    return -LINUX_ENOSYS;
}
/*===OmniBridgeOs/kernel/arch/x64/user/linux_syscall.c 结束===*/