/*===OmniBridgeOs/usr/include/ob/errno.h===*/
#ifndef OB_USER_ERRNO_H
#define OB_USER_ERRNO_H

extern int errno;

#define EPERM    1
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define EBADF    9
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENODEV  19
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define EPIPE   32
#define ERANGE  34
#define ENOSYS  38
#define ENOTEMPTY 39
#define EDEADLK 35
#define ENETUNREACH 101
#define EDQUOT 122

#endif /* OB_USER_ERRNO_H */
/*===OmniBridgeOs/usr/include/ob/errno.h 结束===*/