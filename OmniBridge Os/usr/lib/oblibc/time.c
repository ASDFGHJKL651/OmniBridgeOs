/*===OmniBridgeOs/usr/lib/oblibc/time.c===*/
#include "../../include/ob/time.h"
#include "../../include/ob/ob.h"

time_t time(time_t *t)
{
    int64_t r = ob_syscall0(SYS_OB_GetTime);
    if (r < 0) r = 0;
    if (t) *t = (time_t)r;
    return (time_t)r;
}

int clock_gettime(int clk_id, struct timespec *ts)
{
    (void)clk_id;
    if (!ts) return -1;
    int64_t t = ob_syscall0(SYS_OB_GetTime);
    if (t < 0) t = 0;
    ts->tv_sec = (time_t)t;
    ts->tv_nsec = 0;
    return 0;
}
/*===OmniBridgeOs/usr/lib/oblibc/time.c 结束===*/