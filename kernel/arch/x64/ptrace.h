/*===OmniBridgeOs/kernel/arch/x64/ptrace.h===*/
#ifndef OMNIBRIDGE_PTRACE_H
#define OMNIBRIDGE_PTRACE_H

#include <stdint.h>
#include "task.h"

#define PTRACE_TRACEME    0
#define PTRACE_PEEKDATA   1
#define PTRACE_POKEDATA   2
#define PTRACE_CONT       3
#define PTRACE_ATTACH     4
#define PTRACE_DETACH     5
#define PTRACE_GETREGS    6

#define PTRACE_FLAG_TRACED   0x01
#define PTRACE_FLAG_ATTACHED 0x02

void ptrace_init(void);

int64_t ptrace_syscall(struct task_t *cur, uint64_t request, uint64_t pid,
                       uint64_t addr, uint64_t data);

/* ★ 第 18D 步：核心转储。由 signal.c 在默认动作终止前调用。 */
void core_dump(struct task_t *t, int signum, uint64_t fault_addr);

#endif /* OMNIBRIDGE_PTRACE_H */