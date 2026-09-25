#include "selftest.h"
#include "serial.h"
#include "vmm.h"

/*
 * 内核错误处理功能自检实现。
 *
 * 说明（人工必须审查的点）：
 *
 * 1) 除零测试：
 *      rdx:rax / rcx，rcx = 0 触发 #DE。
 *
 * 2) 页错误测试（PAGE_FAULT）：
 *      读取规范的未映射地址 0x00007FFFFFFF000ULL（用户空间顶部一页）。
 *
 * 3) #GP 测试：加载 GDT 索引 6（TSS 高 8 字节）到 DS。
 *
 * 4) #UD 测试：ud2。
 *
 * 5) #BP 测试：int3。
 *
 * 6) syscall 测试：设置 Linux 风格寄存器约定后执行 syscall。
 *
 * 7) USER_KERNEL_PF 测试：
 *      当前尚无 CPL=3 用户态，无法真实模拟 U/S=1 的页错误。
 *      改为内核态读取一个明确未映射的内核地址，
 *      验证 page_fault_handler 能正确打印 CR2、error code。
 *      真实用户态验证留待进程子系统。
 */

/* ============================================================
 * 1) 除零异常（#DE，vector 0）
 * ============================================================ */
static void trigger_divide_error(void)
{
    serial_printf("\n[SELFTEST] step %d: triggering #DE (div by zero)\n",
                  OB_SELFTEST_DIVIDE);

    __asm__ __volatile__(
        "xor %%rcx, %%rcx\n\t"
        "xor %%rdx, %%rdx\n\t"
        "mov $1, %%rax\n\t"
        "div %%rcx\n\t"
        ::: "rax", "rcx", "rdx", "memory");

    serial_printf("[SELFTEST] ERROR: #DE did not fire (unreachable)\n");
}

/* ============================================================
 * 2) 页错误（#PF，vector 14）
 * ============================================================ */
static void trigger_page_fault(void)
{
    const uint64_t bad_addr = 0x00007FFFFFFF000ULL;

    serial_printf("\n[SELFTEST] step %d: triggering #PF (read %p)\n",
                  OB_SELFTEST_PAGE_FAULT, (void *)(uintptr_t)bad_addr);

    volatile uint64_t *bad = (volatile uint64_t *)(uintptr_t)bad_addr;
    uint64_t v = *bad;

    serial_printf("[SELFTEST] ERROR: #PF did not fire, v=0x%llx (unreachable)\n",
                  (unsigned long long)v);
}

/* ============================================================
 * 3) 通用保护（#GP，vector 13）
 * ============================================================ */
static void trigger_gp(void)
{
    serial_printf("\n[SELFTEST] step %d: triggering #GP (load DS=0x30)\n",
                  OB_SELFTEST_GP);

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
 * ============================================================ */
static void trigger_syscall(void)
{
    serial_printf("\n[SELFTEST] step %d: issuing syscall nr=0x11\n",
                  OB_SELFTEST_SYSCALL);

    __asm__ __volatile__(
        "mov $0x11, %%rax\n\t"
        "mov $0xAA, %%rdi\n\t"
        "mov $0xBB, %%rsi\n\t"
        "mov $0xCC, %%rdx\n\t"
        "mov $0xDD, %%r10\n\t"
        "syscall\n\t"
        ::: "rax", "rdi", "rsi", "rdx", "r10", "rcx", "r11", "memory");

    serial_printf("[SELFTEST] syscall returned (unexpected in step-4)\n");
}

/* ============================================================
 * 7) 模拟用户态访问内核地址的 #PF（人工必须审查）
 * ============================================================
 *
 * 真实 U/S=1 页错误需要 CPL=3 代码运行到用户空间后触碰内核地址。
 * 当前尚无用户态进程子系统，无法真实触发。
 *
 * 本测试采取如下近似：
 *   - 选取一段明确未映射的内核地址（在 KERNEL_IMAGE_BASE 映射范围之外）。
 *   - 内核态读取它，触发 #PF，错误码 U/S=0（kernel 访问）。
 *   - page_fault_handler 会打印 CR2 / error code 解析，
 *     并走"内核态页错误 -> panic"路径。
 *   - 通过打印内容验证 CR2、error code 解析、地址空间判定是否工作。
 *
 * 真实用户态验证（error code U/S=1 + CR2 在内核空间）留待进程子系统。
 */
static void trigger_user_kernel_pf(void)
{
    /* 该地址位于内核高半区之外但仍是规范地址。
     * 内核页表只映射 0xFFFFFFFF80000000 起 256MB，
     * 0xFFFFFFFFE0000000 落在映射范围之外（也超出 VMM_KERNEL_PT_COUNT 覆盖），
     * 因此访问必然触发 not-present 页错误。 */
    const uint64_t unmapped_kernel = 0xFFFFFFFFE0000000ULL;

    serial_printf("\n[SELFTEST] step %d: simulating user->kernel access\n",
                  OB_SELFTEST_USER_KERNEL_PF);
    serial_printf("[SELFTEST]   note: currently running at CPL=0, no user mode.\n");
    serial_printf("[SELFTEST]   reading unmapped kernel addr 0x%llx to exercise "
                  "page_fault_handler diagnostics.\n",
                  (unsigned long long)unmapped_kernel);
    serial_printf("[SELFTEST]   expect: CR2=0x%llx, error P=0/U=0/W=0.\n",
                  (unsigned long long)unmapped_kernel);

    volatile uint64_t *bad = (volatile uint64_t *)(uintptr_t)unmapped_kernel;
    uint64_t v = *bad;

    serial_printf("[SELFTEST] ERROR: #PF did not fire, v=0x%llx (unreachable)\n",
                  (unsigned long long)v);
}

/* ============================================================
 * 统一入口
 * ============================================================ */
void selftest_run(int step)
{
    switch (step) {
    case OB_SELFTEST_NONE:
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

    case OB_SELFTEST_USER_KERNEL_PF:
        trigger_user_kernel_pf();
        break;

    default:
        serial_printf("[SELFTEST] unknown step %d, ignored\n", step);
        return;
    }

    serial_printf("[SELFTEST] step %d returned (should not happen)\n", step);
}