/*===OmniBridgeOs/kernel/arch/x64/user/signal.h===*/
#ifndef OMNIBRIDGE_USER_SIGNAL_H
#define OMNIBRIDGE_USER_SIGNAL_H

#include <stdint.h>
#include "user.h"

struct regs;

#define OB_SIGHUP    1
#define OB_SIGINT    2
#define OB_SIGQUIT   3
#define OB_SIGILL    4
#define OB_SIGTRAP   5
#define OB_SIGABRT   6
#define OB_SIGBUS    7
#define OB_SIGFPE    8
#define OB_SIGKILL   9
#define OB_SIGUSR1  10
#define OB_SIGSEGV  11
#define OB_SIGUSR2  12
#define OB_SIGPIPE  13
#define OB_SIGALRM  14
#define OB_SIGTERM  15
#define OB_SIGCHLD  17
#define OB_SIGCONT  18
#define OB_SIGSTOP  19
#define OB_SIGTSTP  20
#define OB_SIGTTIN  21
#define OB_SIGTTOU  22

#define OB_NSIG     32

#define OB_SIG_DFL  0
#define OB_SIG_IGN  1

typedef void (*ob_sighandler_t)(int);

struct ob_sigaction {
    ob_sighandler_t handler;
    uint64_t       sa_mask;
    uint32_t       sa_flags;
    uint32_t       _pad;
};

struct ob_sigpending {
    uint64_t bits;
    uint64_t blocked;
};

void signal_init(void);
void signal_init_task(struct task_t *t);
void signal_free_task(struct task_t *t);

int64_t sys_signal(int signum, ob_sighandler_t handler);
int64_t sys_sigaction(int signum, const struct ob_sigaction *act,
                      struct ob_sigaction *old);
int64_t sys_sigprocmask(int how, const uint64_t *set, uint64_t *oldset);
int64_t sys_kill(uint64_t pid, int signum);
int64_t sys_raise(int signum);

/* 从异常栈帧递送信号。返回 1：已安装用户态 handler；返回 0：未安装。 */
int signal_from_exception(struct task_t *t, const struct regs *r);

int signal_deliver_if_pending(struct task_t *t);

/* ★ 获取原始上下文（sigreturn 用） */
struct user_regs *signal_resume_slot(struct task_t *t);

/* ★ 新增：获取进入 handler 的上下文（异常处理返回用户态用） */
struct user_regs *signal_handler_slot(struct task_t *t);

/* ★ sigreturn 前修正 resume 上下文（推进 RIP 跳过故障指令） */
int signal_fixup_resume(struct task_t *t);

#endif /* OMNIBRIDGE_USER_SIGNAL_H */
/*===OmniBridgeOs/kernel/arch/x64/user/signal.h 结束===*/