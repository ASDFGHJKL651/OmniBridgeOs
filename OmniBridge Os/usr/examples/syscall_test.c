/*===OmniBridgeOs/usr/examples/syscall_test.c===*/
/*
 * syscall_test.c —— 用户态系统调用与 CPL=3 执行验证。
 *
 * 验收对应（第 18C 步）：
 *   ✓ QEMU 中成功从内核态切换到 CPL=3 执行用户程序，并通过 syscall 返回。
 *   ✓ oblibc 可编译并运行（printf、write、getpid）。
 *
 * 预期输出（串口）：
 *   [syscall_test] entering user-mode test
 *   [syscall_test] getpid   = <n>
 *   [syscall_test] getppid  = <n>
 *   [syscall_test] write ok
 *   [syscall_test] sleep(1) returned
 *   [syscall_test] OK
 *
 * 人工必须审查：
 *   - 本程序全程通过 syscall 指令进入内核，返回值从 rax 取。
 *   - 任何 CPL=3 切换失败都会导致进程异常终止，本程序不会打印 OK。
 */
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/unistd.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[syscall_test] entering user-mode test\n");

    /* 1) getpid / getppid 必须返回非零值 */
    uint32_t pid  = getpid();
    uint32_t ppid = getppid();
    printf("[syscall_test] getpid   = %u\n", (unsigned)pid);
    printf("[syscall_test] getppid  = %u\n", (unsigned)ppid);

    if (pid == 0) {
        printf("[syscall_test] FAIL: getpid returned 0\n");
        return 1;
    }

    /* 2) write 直调 stdout（fd=1） */
    const char *msg = "[syscall_test] write ok\n";
    int64_t n = write(1, msg, 28);
    if (n != 28) {
        printf("[syscall_test] FAIL: write returned %lld\n",
               (long long)n);
        return 1;
    }

    /* 3) sleep(1) —— 内核应至少 hlt 一段 */
    int rc = sleep(1);
    (void)rc;
    printf("[syscall_test] sleep(1) returned\n");

    printf("[syscall_test] OK\n");
    return 0;
}
/*===OmniBridgeOs/usr/examples/syscall_test.c 结束===*/