/*===OmniBridgeOs/usr/include/ob/time.h===*/
#ifndef OB_USER_TIME_H
#define OB_USER_TIME_H

#include <stdint.h>

typedef int64_t time_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

time_t time(time_t *t);
int    clock_gettime(int clk_id, struct timespec *ts);

#endif
/*===OmniBridgeOs/usr/include/ob/time.h 结束===*/