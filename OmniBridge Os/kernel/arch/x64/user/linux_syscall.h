/*===OmniBridgeOs/kernel/arch/x64/user/linux_syscall.h===*/
/*
 * Linux ELF 兼容层：syscall 号、错误码、分发入口（第 19 步）。
 *
 * 关键约束（人工必须审查）：
 *   - Linux syscall ABI：nr=RAX，arg0..5 = RDI/RSI/RDX/R10/R8/R9。
 *     syscall_frame 字段与之对应（f->rdi/rsi/rdx/r10/r8/r9）。
 *   - 返回值：成功为非负，失败为 -errno（errno 为正数）。
 *   - 所有 handler 在执行副作用前必须先经过 check_permission()。
 *   - 未映射的 syscall 必须返回 -ENOSYS 并写审计，绝不假装成功。
 *
 * ★ 第 19 步修复（本轮）：
 *   补全 LINUX_ENETUNREACH (=101)，此前 linux_syscall.c 的
 *   ob_to_linux_errno() 引用了该宏但头文件中未定义，导致编译失败。
 *   本版本同时补齐其余常被引用的 Linux errno 值，避免同类问题再次发生。
 */
#ifndef OMNIBRIDGE_USER_LINUX_SYSCALL_H
#define OMNIBRIDGE_USER_LINUX_SYSCALL_H

#include <stdint.h>
#include "syscall.h"
#include "task.h"

/* ---------- Linux x86_64 syscall 号（本步映射子集） ---------- */
#define LINUX_SYS_read              0
#define LINUX_SYS_write             1
#define LINUX_SYS_open              2
#define LINUX_SYS_close             3
#define LINUX_SYS_stat              4
#define LINUX_SYS_fstat             5
#define LINUX_SYS_lstat             6
#define LINUX_SYS_poll              7
#define LINUX_SYS_lseek             8
#define LINUX_SYS_mmap              9
#define LINUX_SYS_mprotect          10
#define LINUX_SYS_munmap            11
#define LINUX_SYS_brk               12
#define LINUX_SYS_rt_sigaction      13
#define LINUX_SYS_rt_sigprocmask    14
#define LINUX_SYS_rt_sigreturn      15
#define LINUX_SYS_ioctl             16
#define LINUX_SYS_pread64           17
#define LINUX_SYS_pwrite64          18
#define LINUX_SYS_readv             19
#define LINUX_SYS_writev            20
#define LINUX_SYS_access            21
#define LINUX_SYS_pipe              22
#define LINUX_SYS_select            23
#define LINUX_SYS_sched_yield       24
#define LINUX_SYS_mremap            25
#define LINUX_SYS_msync             26
#define LINUX_SYS_madvise           28
#define LINUX_SYS_dup               32
#define LINUX_SYS_dup2              33
#define LINUX_SYS_pause             34
#define LINUX_SYS_nanosleep         35
#define LINUX_SYS_getpid            39
#define LINUX_SYS_sendfile          40
#define LINUX_SYS_socket            41
#define LINUX_SYS_connect           42
#define LINUX_SYS_accept            43
#define LINUX_SYS_sendto            44
#define LINUX_SYS_recvfrom          45
#define LINUX_SYS_bind              49
#define LINUX_SYS_listen            50
#define LINUX_SYS_clone             56
#define LINUX_SYS_fork              57
#define LINUX_SYS_vfork             58
#define LINUX_SYS_execve            59
#define LINUX_SYS_exit              60
#define LINUX_SYS_wait4             61
#define LINUX_SYS_kill              62
#define LINUX_SYS_uname             63
#define LINUX_SYS_fcntl             72
#define LINUX_SYS_getcwd            79
#define LINUX_SYS_chdir             80
#define LINUX_SYS_mkdir             83
#define LINUX_SYS_rmdir             84
#define LINUX_SYS_unlink            87
#define LINUX_SYS_readlink          89
#define LINUX_SYS_chmod             90
#define LINUX_SYS_getuid            102
#define LINUX_SYS_getgid            104
#define LINUX_SYS_geteuid           107
#define LINUX_SYS_getegid           108
#define LINUX_SYS_setpgid           109
#define LINUX_SYS_getppid           110
#define LINUX_SYS_setsid            112
#define LINUX_SYS_getpgid           121
#define LINUX_SYS_arch_prctl        158
#define LINUX_SYS_gettid            186
#define LINUX_SYS_readahead         187
#define LINUX_SYS_futex             202
#define LINUX_SYS_set_tid_address   218
#define LINUX_SYS_clock_gettime     228
#define LINUX_SYS_clock_getres      229
#define LINUX_SYS_exit_group        231
#define LINUX_SYS_openat            257
#define LINUX_SYS_newfstatat        262
#define LINUX_SYS_readlinkat        267
#define LINUX_SYS_pipe2             293
#define LINUX_SYS_prlimit64         302
#define LINUX_SYS_getrandom         318
#define LINUX_SYS_statx             332
#define LINUX_SYS_rseq              334

/* ============================================================
 * Linux errno（正数；返回时取负）
 *
 * 值取自 Linux x86_64 <asm-generic/errno-base.h> 与 <asm-generic/errno.h>。
 * 人工必须审查：
 *   - 严禁将这些值与内核 OB_* 错误码混用。
 *   - 若在 handler 中返回负值，必须是 -LINUX_E* 形式。
 *   - linux_syscall.c 的 ob_to_linux_errno() 只应引用此处定义的宏。
 * ============================================================ */
#define LINUX_EPERM          1   /* Operation not permitted */
#define LINUX_ENOENT         2   /* No such file or directory */
#define LINUX_ESRCH          3   /* No such process */
#define LINUX_EINTR          4   /* Interrupted system call */
#define LINUX_EIO            5   /* I/O error */
#define LINUX_ENXIO          6   /* No such device or address */
#define LINUX_E2BIG          7   /* Argument list too long */
#define LINUX_ENOEXEC        8   /* Exec format error */
#define LINUX_EBADF          9   /* Bad file number */
#define LINUX_ECHILD        10   /* No child processes */
#define LINUX_EAGAIN        11   /* Try again */
#define LINUX_ENOMEM        12   /* Out of memory */
#define LINUX_EACCES        13   /* Permission denied */
#define LINUX_EFAULT        14   /* Bad address */
#define LINUX_ENOTBLK       15   /* Block device required */
#define LINUX_EBUSY         16   /* Device or resource busy */
#define LINUX_EEXIST        17   /* File exists */
#define LINUX_EXDEV         18   /* Cross-device link */
#define LINUX_ENODEV        19   /* No such device */
#define LINUX_ENOTDIR       20   /* Not a directory */
#define LINUX_EISDIR        21   /* Is a directory */
#define LINUX_EINVAL        22   /* Invalid argument */
#define LINUX_ENFILE        23   /* File table overflow */
#define LINUX_EMFILE        24   /* Too many open files */
#define LINUX_ENOTTY        25   /* Not a typewriter */
#define LINUX_ETXTBSY       26   /* Text file busy */
#define LINUX_EFBIG         27   /* File too large */
#define LINUX_ENOSPC        28   /* No space left on device */
#define LINUX_ESPIPE        29   /* Illegal seek */
#define LINUX_EROFS         30   /* Read-only file system */
#define LINUX_EMLINK        31   /* Too many links */
#define LINUX_EPIPE         32   /* Broken pipe */
#define LINUX_EDOM          33   /* Math argument out of domain */
#define LINUX_ERANGE        34   /* Math result not representable */
#define LINUX_EDEADLK       35   /* Resource deadlock would occur */
#define LINUX_ENAMETOOLONG  36   /* File name too long */
#define LINUX_ENOLCK        37   /* No record locks available */
#define LINUX_ENOSYS        38   /* Function not implemented */
#define LINUX_ENOTEMPTY     39   /* Directory not empty */
#define LINUX_ELOOP         40   /* Too many symbolic links */
#define LINUX_ENOMSG        42   /* No message of desired type */
#define LINUX_EIDRM         43   /* Identifier removed */
#define LINUX_ENOSTR        60   /* Device not a stream */
#define LINUX_ENODATA       61   /* No data available */
#define LINUX_ETIME         62   /* Timer expired */
#define LINUX_ENOSR         63   /* Out of streams resources */
#define LINUX_EPROTO        71   /* Protocol error */
#define LINUX_EMULTIHOP     72   /* Multihop attempted */
#define LINUX_EBADMSG       74   /* Not a data message */
#define LINUX_EOVERFLOW     75   /* Value too large for defined type */
#define LINUX_EILSEQ        84   /* Illegal byte sequence */
#define LINUX_EUSERS        87   /* Too many users */
#define LINUX_ENOTSOCK      88   /* Socket operation on non-socket */
#define LINUX_EDESTADDRREQ  89   /* Destination address required */
#define LINUX_EMSGSIZE      90   /* Message too long */
#define LINUX_EPROTOTYPE    91   /* Protocol wrong type for socket */
#define LINUX_ENOPROTOOPT   92   /* Protocol not available */
#define LINUX_EPROTONOSUPPORT 93 /* Protocol not supported */
#define LINUX_ESOCKTNOSUPPORT 94 /* Socket type not supported */
#define LINUX_EOPNOTSUPP    95   /* Operation not supported on transport endpoint */
#define LINUX_ENOTSUP       LINUX_EOPNOTSUPP  /* 别名 */
#define LINUX_EPFNOSUPPORT  96   /* Protocol family not supported */
#define LINUX_EAFNOSUPPORT  97   /* Address family not supported */
#define LINUX_EADDRINUSE    98   /* Address already in use */
#define LINUX_EADDRNOTAVAIL 99   /* Cannot assign requested address */
#define LINUX_ENETDOWN     100   /* Network is down */
#define LINUX_ENETUNREACH  101   /* Network is unreachable */     /* ★ 本次修复 */
#define LINUX_ENETRESET    102   /* Network dropped connection */
#define LINUX_ECONNABORTED 103   /* Software caused connection abort */
#define LINUX_ECONNRESET   104   /* Connection reset by peer */
#define LINUX_ENOBUFS      105   /* No buffer space available */
#define LINUX_EISCONN      106   /* Transport endpoint already connected */
#define LINUX_ENOTCONN     107   /* Transport endpoint not connected */
#define LINUX_ESHUTDOWN    108   /* Cannot send after transport shutdown */
#define LINUX_ETOOMANYREFS 109   /* Too many references */
#define LINUX_ETIMEDOUT    110   /* Connection timed out */
#define LINUX_ECONNREFUSED 111   /* Connection refused */
#define LINUX_EHOSTDOWN    112   /* Host is down */
#define LINUX_EHOSTUNREACH 113   /* No route to host */
#define LINUX_EALREADY     114   /* Operation already in progress */
#define LINUX_EINPROGRESS  115   /* Operation now in progress */
#define LINUX_ESTALE       116   /* Stale NFS file handle */
#define LINUX_EDQUOT       122   /* Disk quota exceeded */
#define LINUX_ECANCELED    125   /* Operation canceled */
#define LINUX_ENOTSUPP     524   /* (Rust/后端专用，保留兼容) */

/* AT_* 常量（用于 arch_prctl / auxv） */
#define LINUX_ARCH_SET_GS 0x1001
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003
#define LINUX_ARCH_GET_GS 0x1004

/* ---------- 接口 ---------- */
void linux_syscall_init(void);

/* OB 错误码 -> Linux 错误码（正数）。
 * 输入应为一个负的 OB_* 值；返回正数 errno。
 * 例如 ob_to_linux_errno(OB_EPERM=-1) == LINUX_EPERM=1 */
int ob_to_linux_errno(int ob_err);

/* Linux 兼容 syscall 分发器。返回 Linux 惯例值（>=0 成功，<0 为 -errno）。 */
int64_t linux_syscall_dispatch(struct task_t *cur, struct syscall_frame *f);

#endif /* OMNIBRIDGE_USER_LINUX_SYSCALL_H */
/*===OmniBridgeOs/kernel/arch/x64/user/linux_syscall.h 结束===*/