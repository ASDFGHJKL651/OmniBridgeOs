/*===OmniBridgeOs/usr/include/ob/unistd.h===*/
#ifndef OB_USER_UNISTD_H
#define OB_USER_UNISTD_H

#include <stdint.h>
#include <stddef.h>

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

int64_t read(int fd, void *buf, uint64_t count);
int64_t write(int fd, const void *buf, uint64_t count);
int     close(int fd);
int     open(const char *path, int flags, ...);

/* ★ 第 18D 步：pipe —— 创建匿名管道。
 *   fds[0] 为读端，fds[1] 为写端。
 *   成功返回 0；失败返回 -1 并设置 errno。 */
int     pipe(int fds[2]);

uint32_t getpid(void);
uint32_t getppid(void);

int     sleep(unsigned seconds);
int     usleep(unsigned usec);

void    _exit(int code) __attribute__((noreturn));

#endif /* OB_USER_UNISTD_H */
/*===OmniBridgeOs/usr/include/ob/unistd.h 结束===*/