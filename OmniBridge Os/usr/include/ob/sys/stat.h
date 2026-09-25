#ifndef OB_USER_SYS_STAT_H
#define OB_USER_SYS_STAT_H
#include <stdint.h>
#include "../sys/types.h"
struct stat {
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_size;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};
#endif