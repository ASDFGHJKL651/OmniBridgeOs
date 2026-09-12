#ifndef OMNIBRIDGE_SELFTEST_H
#define OMNIBRIDGE_SELFTEST_H

/*
 * 内核错误处理功能自检。
 *
 * 通过编译期常量 OB_SELFTEST_STEP 选择要触发的路径：
 *   NONE       0  不测试，selftest_run 直接返回
 *   DIVIDE     1  #DE  除零异常（vector 0）
 *   PAGE_FAULT 2  #PF  页错误（vector 14，读未映射页）
 *   GP         3  #GP  通用保护（vector 13，加载非法段选择子）
 *   INVALID_OP 4  #UD  无效操作码（vector 6，ud2）
 *   BREAKPOINT 5  #BP  断点（vector 3，int3）
 *   SYSCALL    6  syscall 指令骨架路径（进入 syscall_entry）
 *
 * 除 NONE 与 SYSCALL 之外的所有测试，一旦触发都会停在
 * exception_handler 的 hlt 死循环里，必须重启 QEMU 才能做下一项。
 *
 * 在 main.c 中使用：
 *     #include "selftest.h"
 *     ...
 *     selftest_run(OB_SELFTEST_STEP);
 *
 * 未指定 OB_SELFTEST_STEP 时默认为 NONE（即正常启动，不做任何测试）。
 */

#define OB_SELFTEST_NONE          0
#define OB_SELFTEST_DIVIDE        1
#define OB_SELFTEST_PAGE_FAULT    2
#define OB_SELFTEST_GP            3
#define OB_SELFTEST_INVALID_OP    4
#define OB_SELFTEST_BREAKPOINT    5
#define OB_SELFTEST_SYSCALL       6

#ifndef OB_SELFTEST_STEP
#define OB_SELFTEST_STEP OB_SELFTEST_NONE
#endif

/*
 * 执行一个自检步骤。
 *   step == NONE：立即返回。
 *   step != NONE：触发对应路径。
 *     - DIVIDE / PAGE_FAULT / GP / INVALID_OP / BREAKPOINT
 *         → 不返回（异常处理进入 hlt 循环）
 *     - SYSCALL
 *         → 进入 syscall_entry；当前 GDT 下 SYSRET 会触发 #GP，
 *           因此正常情况下也不会返回到调用点。
 */
void selftest_run(int step);

#endif /* OMNIBRIDGE_SELFTEST_H */