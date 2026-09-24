#ifndef OMNIBRIDGE_SELFTEST_H
#define OMNIBRIDGE_SELFTEST_H

/*
 * 内核错误处理功能自检。
 *
 * 通过编译期常量 OB_SELFTEST_STEP 选择要触发的路径：
 *   NONE            0  不测试，selftest_run 直接返回
 *   DIVIDE          1  #DE  除零异常（vector 0）
 *   PAGE_FAULT      2  #PF  页错误（vector 14，读未映射页）
 *   GP              3  #GP  通用保护（vector 13，加载非法段选择子）
 *   INVALID_OP      4  #UD  无效操作码（vector 6，ud2）
 *   BREAKPOINT      5  #BP  断点（vector 3，int3）
 *   SYSCALL         6  syscall 指令骨架路径（进入 syscall_entry）
 *   USER_KERNEL_PF  7  模拟用户态访问内核地址的 #PF 路径
 *                     （当前无 CPL=3，用内核读未映射内核地址代替，
 *                      验证 page_fault_handler 的诊断输出）
 *
 * 除 NONE 与 SYSCALL 之外的所有测试，一旦触发都会停在
 * exception_handler / page_fault_handler 的 hlt 死循环里。
 */

#define OB_SELFTEST_NONE            0
#define OB_SELFTEST_DIVIDE          1
#define OB_SELFTEST_PAGE_FAULT      2
#define OB_SELFTEST_GP              3
#define OB_SELFTEST_INVALID_OP      4
#define OB_SELFTEST_BREAKPOINT      5
#define OB_SELFTEST_SYSCALL         6
#define OB_SELFTEST_USER_KERNEL_PF  7

#ifndef OB_SELFTEST_STEP
#define OB_SELFTEST_STEP OB_SELFTEST_NONE
#endif

void selftest_run(int step);

#endif /* OMNIBRIDGE_SELFTEST_H */