/*===OmniBridgeOs/usr/include/ob/signal.h===*/
#ifndef OB_USER_SIGNAL_H
#define OB_USER_SIGNAL_H

#include <stdint.h>

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20

/* ---------- 特殊 handler 值 ----------
 *
 * SIG_DFL  : 采用默认动作（内核视为终止进程）。
 * SIG_IGN  : 忽略该信号。
 * SIG_ERR  : signal() 返回值，表示注册失败（符合 POSIX 惯例，
 *            即 (sighandler_t)-1）。用户代码通过 `if (old == SIG_ERR)`
 *            判断注册是否成功。
 *
 * 人工必须审查：
 *   - 内核 sys_signal 成功时返回旧 handler（可能为 NULL 或任意用户态
 *     函数地址），失败时返回负错误码（如 -EINVAL）。
 *   - oblibc 的 signal() 包装据此把负错误码映射为 SIG_ERR，
 *     把成功路径的旧 handler 原样返回。
 */
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    uint64_t     sa_mask;
    uint32_t     sa_flags;
    uint32_t     _pad;
};

sighandler_t signal(int signum, sighandler_t handler);
int          sigaction(int signum, const struct sigaction *act,
                       struct sigaction *old);
int          kill(uint32_t pid, int signum);
int          raise(int signum);

#endif /* OB_USER_SIGNAL_H */
/*===OmniBridgeOs/usr/include/ob/signal.h 结束===*/