/*===OmniBridgeOs/usr/lib/oblibc/signal.c===*/
#include "../../include/ob/signal.h"
#include "../../include/ob/ob.h"
#include <stdint.h>

sighandler_t signal(int signum, sighandler_t handler)
{
    int64_t r = ob_syscall2(SYS_OB_Sigaction, (uint64_t)signum,
                            (uint64_t)(uintptr_t)handler);
    return (sighandler_t)(uintptr_t)r;
}

int sigaction(int signum, const struct sigaction *act,
              struct sigaction *old)
{
    if (act) {
        int64_t r = ob_syscall2(SYS_OB_Sigaction,
                                (uint64_t)signum,
                                (uint64_t)(uintptr_t)act->sa_handler);
        if (r < 0) return -1;
    }
    if (old) {
        old->sa_handler = (sighandler_t)0;
        old->sa_mask = 0;
        old->sa_flags = 0;
    }
    return 0;
}

int kill(uint32_t pid, int signum)
{
    int64_t r = ob_syscall2(SYS_OB_Kill, (uint64_t)pid, (uint64_t)signum);
    return (int)r;
}

int raise(int signum)
{
    uint32_t pid = (uint32_t)ob_syscall0(SYS_OB_Getpid);
    int64_t r = ob_syscall2(SYS_OB_Kill, (uint64_t)pid, (uint64_t)signum);
    return (int)r;
}
/*===OmniBridgeOs/usr/lib/oblibc/signal.c 结束===*/