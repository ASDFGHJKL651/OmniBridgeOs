/*===OmniBridgeOs/kernel/arch/x64/user/tty.h===*/
#ifndef OMNIBRIDGE_USER_TTY_H
#define OMNIBRIDGE_USER_TTY_H

#include <stdint.h>
#include "task.h"

#define TTY_MAX_PGRP 16
#define TTY_MAX_JOBS 16

struct tty_job {
    uint64_t pgid;
    uint64_t leader_pid;
    int      state;         /* 0=stopped, 1=running, 2=done */
    char     name[64];
};

struct tty {
    uint64_t foreground_pgid;
    uint64_t foreground_leader;
    uint64_t my_pgid;         /* shell 自身 pgid */
    struct tty_job jobs[TTY_MAX_JOBS];
    uint32_t job_count;
    uint32_t next_job_id;
};

void tty_init(void);

/* 从串口读取一个字节；由 OShell 和用户程序共享（简化：非阻塞返回 -1）。 */
int  tty_getchar(void);

/* 前台进程组控制 */
int64_t sys_tcsetpgrp(uint64_t pgid);
int64_t sys_tcgetpgrp(void);

/* 由串口中断或 OShell 触发：解析 Ctrl-C / Ctrl-Z。 */
void tty_handle_char(int c);

/* 作业控制 */
int tty_add_job(const char *name, uint64_t pgid, uint64_t leader_pid);
int tty_list_jobs(void);
int tty_fg(int job_id);
int tty_bg(int job_id);

#endif /* OMNIBRIDGE_USER_TTY_H */
/*===OmniBridgeOs/kernel/arch/x64/user/tty.h 结束===*/