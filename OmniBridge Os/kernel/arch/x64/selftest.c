#include "selftest.h"
#include "serial.h"

/*
 * 内核错误处理功能自检实现。
 *
 * 说明（人工必须审查的点）：
 *
 * 1) 除零测试：
 *      rdx:rax / rcx，rcx = 0 触发 #DE。
 *      用内联汇编写，避免编译器在优化期做常量折叠把 div 删掉。
 *
 * 2) 页错误测试：
 *      读取一个规范的、未映射的用户空间地址。
 *      恒等映射只覆盖 0..4GB，DirectMap 覆盖 0xFFFF8000_00000000 起，
 *      内核高半区映射在 0xFFFFFFFF_80000000 起。
 *      这里选 0x0000_7FFF_FFFF_F000（用户空间顶部一页），规范且未映射。
 *
 * 3) #GP 测试：
 *      加载 GDT 索引 6（TSS 高 8 字节）到 DS。这不是合法的数据段
 *      选择子，CPU 抛 #GP，并向栈压入 error code（选择子 0x30）。
 *
 * 4) #UD 测试：
 *      ud2 是 Intel 定义的"保证触发"无效操作码，专门用于此类测试。
 *
 * 5) #BP 测试：
 *      int3 是单字节断点指令，进入 vector 3。
 *
 * 6) syscall 测试：
 *      设置 Linux 风格寄存器约定后执行 syscall。
 *      当前 GDT 中 user code = 0x18、user data = 0x20，
 *      而 syscall_init 中 STAR[63:48] 填的是 0x1B。SYSRET 会按
 *      CS = STAR[63:48] + 16 = 0x2B 返回，那是 GDT 索引 5（TSS 描述
 *      符，非代码段），必然 #GP。这正是第 4 步"尚无用户态"的已知
 *      限制——本测试只用于验证 SYSCALL 入口被正确调用、参数被正确
 *      分发到 syscall_dispatcher。
 */

/* ============================================================
 * 1) 除零异常（#DE，vector 0）
 * ============================================================ */
static void trigger_divide_error(void)
{
    serial_printf("\n[SELFTEST] step %d: triggering #DE (div by zero)\n",
                  OB_SELFTEST_DIVIDE);

    /* rcx = 0；rdx:rax = 0:1；div rcx → #DE */
    __asm__ __volatile__(
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "mov $1, %%rax\n\t"
        "div %%rcx\n\t"
        ::: "rax", "rcx", "rdx", "memory");

    /* 若走到这里说明 #DE 未触发，异常路径有问题 */
    serial_printf("[SELFTEST] ERROR: #DE did not fire (unreachable)\n");
}

/* ============================================================
 * 2) 页错误（#PF，vector 14）
 * ============================================================ */
static void trigger_page_fault(void)
{
    const uint64_t bad_addr = 0x00007FFFFFFF000ULL;   /* 用户空间顶部一页 */

    serial_printf("\n[SELFTEST] step %d: triggering #PF (read %p)\n",
                  OB_SELFTEST_PAGE_FAULT, (void *)(uintptr_t)bad_addr);

    /* volatile 防止编译器把这次读消除掉 */
    volatile uint64_t *bad = (volatile uint64_t *)(uintptr_t)bad_addr;
    uint64_t v = *bad;

    serial_printf("[SELFTEST] ERROR: #PF did not fire, v=0x%llx (unreachable)\n",
                  (unsigned long long)v);
}

/* ============================================================
 * 3) 通用保护（#GP，vector 13，带 error code）
 * ============================================================ */
static void trigger_gp(void)
{
    serial_printf("\n[SELFTEST] step %d: triggering #GP (load DS=0x30)\n",
                  OB_SELFTEST_GP);

    /* 0x30 = GDT 索引 6，TSS 描述符的高 8 字节，非数据段 → #GP */
    __asm__ __volatile__(
        "mov $0x30, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        ::: "ax", "memory");

    serial_printf("[SELFTEST] ERROR: #GP did not fire (unreachable)\n");
}

/* ============================================================
 * 4) 无效操作码（#UD，vector 6）
 * ============================================================ */
static void trigger_invalid_opcode(void)
{
    serial_printf("\n[SELFTEST] step %d: triggering #UD (ud2)\n",
                  OB_SELFTEST_INVALID_OP);

    __asm__ __volatile__("ud2" ::: "memory");

    serial_printf("[SELFTEST] ERROR: #UD did not fire (unreachable)\n");
}

/* ============================================================
 * 5) 断点（#BP，vector 3）
 * ============================================================ */
static void trigger_breakpoint(void)
{
    serial_printf("\n[SELFTEST] step %d: triggering #BP (int3)\n",
                  OB_SELFTEST_BREAKPOINT);

    __asm__ __volatile__("int3" ::: "memory");

    serial_printf("[SELFTEST] ERROR: #BP did not fire (unreachable)\n");
}

/* ============================================================
 * 6) 系统调用骨架
 * ============================================================
 *
 * 寄存器约定（Linux 风格，与 syscall_dispatcher 中的打印一致）：
 *     rax = 系统调用号
 *     rdi, rsi, rdx, r10, r8, r9 = 参数 1..6
 *
 * 硬件行为：
 *     syscall 会把返回 RIP 写入 rcx，把当前 RFLAGS 写入 r11，
 *     并按 STAR 中设置的目标 CS/SS 切换特权级。当前尚无用户态，
 *     因此这些寄存器保存的都是内核态值。
 *
 * 预期结果：
 *     syscall_entry → syscall_dispatcher 打印 nr=0x11, arg0..arg3；
 *     sysretq 因 CS=0x2B 无效触发 #GP，被 exception_handler 捕获。
 */
static void trigger_syscall(void)
{
    serial_printf("\n[SELFTEST] step %d: issuing syscall nr=0x11\n",
                  OB_SELFTEST_SYSCALL);

    __asm__ __volatile__(
        "mov $0x11, %%rax\n\t"       /* nr = 17 */
        "mov $0xAA, %%rdi\n\t"       /* arg0 */
        "mov $0xBB, %%rsi\n\t"       /* arg1 */
        "mov $0xCC, %%rdx\n\t"       /* arg2 */
        "mov $0xDD, %%r10\n\t"       /* arg3 */
        "syscall\n\t"
        ::: "rax", "rdi", "rsi", "rdx", "r10", "rcx", "r11", "memory");

    serial_printf("[SELFTEST] syscall returned (unexpected in step-4)\n");
}

/* ============================================================
 * 统一入口
 * ============================================================ */
void selftest_run(int step)
{
    switch (step) {
    case OB_SELFTEST_NONE:
        /* 无测试，正常继续 */
        return;

    case OB_SELFTEST_DIVIDE:
        trigger_divide_error();
        break;

    case OB_SELFTEST_PAGE_FAULT:
        trigger_page_fault();
        break;

    case OB_SELFTEST_GP:
        trigger_gp();
        break;

    case OB_SELFTEST_INVALID_OP:
        trigger_invalid_opcode();
        break;

    case OB_SELFTEST_BREAKPOINT:
        trigger_breakpoint();
        break;

    case OB_SELFTEST_SYSCALL:
        trigger_syscall();
        break;

    default:
        serial_printf("[SELFTEST] unknown step %d, ignored\n", step);
        return;
    }

    /* 走到这里说明异常路径没有按预期停住，记录一下以便定位 */
    serial_printf("[SELFTEST] step %d returned (should not happen)\n", step);
}