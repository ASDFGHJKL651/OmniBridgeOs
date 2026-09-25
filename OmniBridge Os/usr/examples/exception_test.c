/*===OmniBridgeOs/usr/examples/exception_test.c===*/
/*
 * exception_test.c —— 用户态非法内存访问验收。
 *
 * 验收对应（第 18C 步）：
 *   ✓ 用户态触发异常（#PF、#GP 等）时，内核应能捕获并转换为信号，
 *     而不是 panic。
 *   ✓ 用户态异常不得导致内核 panic，必须转换为信号。
 *
 * ★ 本轮修复：
 *   原代码写 `0 /* OB_RES_MEMORY *`，但内核 permission.h 中
 *   `OB_RES_FILE = 0`、`OB_RES_MEMORY = 1`。因此内核把它当成
 *   文件资源检查，对 KERNEL_PROBE_ADDR 这个"路径字符串"返回 0
 *   （因为空路径被 permission_check_file 当普通路径放行），
 *   从而打印 `FAIL: kernel probe not denied (r=1)`。
 *
 *   修复：改用 ob.h 导出的 OB_RES_MEMORY 常量，杜绝硬编码数字。
 *
 * 预期输出（串口，在崩溃前）：
 *   [exception_test] check_native(/kernel/) -> DENIED
 *   [exception_test] triggering user-mode #PF
 *   [PF] user-mode #PF -> SIGSEGV pid=<n>
 *   [SIGNAL] default action sig=11 ...
 *   [TASK] exit pid=<n> code=-11
 */
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"

/* 内核地址：DirectMap 起点 */
#define KERNEL_PROBE_ADDR   0xFFFF800000000000ULL
/* 用户态明显未映射的地址（靠近 0 的未映射低页） */
#define UNMAPPED_USER_ADDR  0x0000000000000100ULL

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* A) 权限查询：对内核地址访问应为 DENIED */
    int64_t r = ob_syscall4(SYS_OB_CheckAccessNative,
                            OB_RES_MEMORY,               /* ★ 修复：原来写死为 0 */
                            KERNEL_PROBE_ADDR,
                            (uint64_t)OB_ACCESS_READ,
                            0);

    /* 内核约定：0 = DENIED，1 = ALLOWED */
    if (r == OB_PERM_DENIED) {
        printf("[exception_test] check_native(/kernel/) -> DENIED\n");
    } else {
        printf("[exception_test] FAIL: kernel probe not denied (r=%lld)\n",
               (long long)r);
        return 1;
    }

    /* B) 直接触发用户态页错误 */
    printf("[exception_test] triggering user-mode #PF\n");
    volatile unsigned long *bad =
        (volatile unsigned long *)(unsigned long)UNMAPPED_USER_ADDR;
    volatile unsigned long v = *bad;
    (void)v;

    /* 若内核未把异常转为信号，执行不会到达这里 */
    printf("[exception_test] FAIL: #PF not converted to signal\n");
    return 1;
}
/*===OmniBridgeOs/usr/examples/exception_test.c 结束===*/