/*===OmniBridgeOs/kernel/arch/x64/user/tty.c===*/
#include "tty.h"
#include "signal.h"
#include "serial.h"
#include "task.h"
#include "sched.h"

static struct tty g_tty;

void tty_init(void)
{
    uint8_t *p = (uint8_t *)&g_tty;
    for (unsigned i = 0; i < sizeof(g_tty); ++i) p[i] = 0;
    g_tty.foreground_pgid = 0;
    g_tty.next_job_id = 1;
    serial_printf("[TTY] init: serial console, jobs=%u\n", TTY_MAX_JOBS);
}

int tty_getchar(void) { return -1; }

static void send_to_pgrp(uint64_t pgid, int signum)
{
    struct task_t *t = 0;
    /* 简化：遍历 task 列表，pgid 以 leader_pid 近似。 */
    /* 当前实现仅向 pid==pgid 的进程及其直接子进程发送。 */
    t = task_find_by_pid(pgid);
    if (!t) return;
    struct sig_state *s = 0;
    (void)s;
    /* 通过 sys_kill 路径 */
    extern int64_t sys_kill(uint64_t pid, int signum);
    sys_kill(pgid, signum);
}

void tty_handle_char(int c)
{
    if (c == 3) {           /* Ctrl-C */
        serial_printf("[TTY] Ctrl-C -> SIGINT to pgid=%llu\n",
                      (unsigned long long)g_tty.foreground_pgid);
        if (g_tty.foreground_pgid)
            send_to_pgrp(g_tty.foreground_pgid, OB_SIGINT);
    } else if (c == 26) {   /* Ctrl-Z */
        serial_printf("[TTY] Ctrl-Z -> SIGTSTP to pgid=%llu\n",
                      (unsigned long long)g_tty.foreground_pgid);
        if (g_tty.foreground_pgid)
            send_to_pgrp(g_tty.foreground_pgid, OB_SIGTSTP);
        /* 标记 job 停止 */
        for (uint32_t i = 0; i < g_tty.job_count; ++i) {
            if (g_tty.jobs[i].pgid == g_tty.foreground_pgid) {
                g_tty.jobs[i].state = 0;
                break;
            }
        }
    }
}

int64_t sys_tcsetpgrp(uint64_t pgid)
{
    g_tty.foreground_pgid = pgid;
    g_tty.foreground_leader = pgid;
    return 0;
}

int64_t sys_tcgetpgrp(void)
{
    return (int64_t)g_tty.foreground_pgid;
}

int tty_add_job(const char *name, uint64_t pgid, uint64_t leader_pid)
{
    if (g_tty.job_count >= TTY_MAX_JOBS) return -1;
    struct tty_job *j = &g_tty.jobs[g_tty.job_count++];
    j->pgid = pgid;
    j->leader_pid = leader_pid;
    j->state = 1;
    int i = 0;
    if (name) while (name[i] && i < 63) { j->name[i] = name[i]; ++i; }
    j->name[i] = '\0';
    return 0;
}

int tty_list_jobs(void)
{
    serial_printf("[TTY] jobs: %u\n", (unsigned)g_tty.job_count);
    for (uint32_t i = 0; i < g_tty.job_count; ++i) {
        const char *st = (g_tty.jobs[i].state == 0) ? "Stopped" :
                         (g_tty.jobs[i].state == 1) ? "Running" : "Done";
        serial_printf("  [%u] pgid=%llu %s %s\n",
                      (unsigned)(i + 1),
                      (unsigned long long)g_tty.jobs[i].pgid,
                      st, g_tty.jobs[i].name);
    }
    return 0;
}

int tty_fg(int job_id)
{
    if (job_id < 1 || (uint32_t)job_id > g_tty.job_count) return -1;
    struct tty_job *j = &g_tty.jobs[job_id - 1];
    j->state = 1;
    g_tty.foreground_pgid = j->pgid;
    g_tty.foreground_leader = j->leader_pid;
    extern int64_t sys_kill(uint64_t pid, int signum);
    sys_kill(j->pgid, OB_SIGCONT);
    return 0;
}

int tty_bg(int job_id)
{
    if (job_id < 1 || (uint32_t)job_id > g_tty.job_count) return -1;
    struct tty_job *j = &g_tty.jobs[job_id - 1];
    j->state = 1;
    extern int64_t sys_kill(uint64_t pid, int signum);
    sys_kill(j->pgid, OB_SIGCONT);
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/user/tty.c 结束===*/