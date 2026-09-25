/*===OmniBridgeOs/usr/lib/oblibc/syscall.c===*/
#include "../../include/ob/ob.h"
#include "../../include/ob/errno.h"
#include "../../include/ob/unistd.h"

int errno = 0;

int64_t read(int fd, void *buf, uint64_t count)
{
    int64_t r = ob_syscall3(SYS_OB_ReadFile, (uint64_t)fd,
                            (uint64_t)(uintptr_t)buf, count);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

int64_t write(int fd, const void *buf, uint64_t count)
{
    int64_t r = ob_syscall3(SYS_OB_WriteFile, (uint64_t)fd,
                            (uint64_t)(uintptr_t)buf, count);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

int open(const char *path, int flags, ...)
{
    int64_t r = ob_syscall2(SYS_OB_OpenFile,
                            (uint64_t)(uintptr_t)path,
                            (uint64_t)(unsigned)flags);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

int close(int fd)
{
    int64_t r = ob_syscall1(SYS_OB_CloseHandle, (uint64_t)fd);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

/* ★ 第 18D 步：pipe 封装。
 *
 * 人工必须审查：
 *   - 内核 SYS_OB_Pipe 返回 0 成功，负值错误。
 *   - 用户态 fds[] 由内核直接写入（内核通过 vmm_get_pte 访问用户页）。
 *   - 必须保证 fds 缓冲区是两个 int 的连续空间。 */
int pipe(int fds[2])
{
    int64_t r = ob_syscall1(SYS_OB_Pipe, (uint64_t)(uintptr_t)fds);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

uint32_t getpid(void)
{
    return (uint32_t)ob_syscall0(SYS_OB_Getpid);
}

uint32_t getppid(void)
{
    return (uint32_t)ob_syscall0(SYS_OB_Getppid);
}

int sleep(unsigned seconds)
{
    return (int)ob_syscall1(SYS_OB_Sleep, (uint64_t)seconds);
}

int usleep(unsigned usec)
{
    return (int)ob_syscall1(SYS_OB_Sleep, (uint64_t)(usec / 1000000 + 1));
}

void _exit(int code)
{
    ob_syscall1(SYS_OB_UserExit, (uint64_t)code);
    for (;;) __asm__ __volatile__("hlt");
}
/*===OmniBridgeOs/usr/lib/oblibc/syscall.c 结束===*/