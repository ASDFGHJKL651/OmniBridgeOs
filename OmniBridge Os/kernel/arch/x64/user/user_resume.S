/*===OmniBridgeOs/kernel/arch/x64/user/user_resume.S===*/
/*
 * user_resume_asm —— 从内核态 iretq 回用户态。
 *
 * ★★★ 第 18C 步关键修复 ★★★
 *
 * 背景（人工必须审查）：
 *   原 user_resume 使用 C inline asm：
 *     "movq %[r15], %%r15\n\t"
 *     ...
 *     "movq %[rip], %%rip\n\t"
 *   20 个 "m" 约束让编译器计算 r->r15..r->rip 的地址。
 *   但 asm 修改了 R15..R8/RSI/RDI/RBP/RDX/RCX/RBX/RAX 等 15 个寄存器，
 *   而 clobber list 只声明了 "memory"。
 *
 *   结果：编译器可能把 r 的基址存放在某个被 asm 修改的寄存器中，
 *   asm 内部后续指令便使用已被覆写的寄存器作为基址，读取错误内存
 *   地址（可能落在用户空间），触发 SMAP #PF。
 *
 *   症状（serial.log）：
 *     [SIGNAL] deliver sig=8 ... handler=0x80000010b0
 *     [PF] Page Fault: CR2=0x8000002c88 err=0x1 (kernel read user page)
 *     [PF] RIP=0xffffffff800402b1 CS=0x8 RFLAGS=0x10007
 *     [PF] Kernel-mode page fault, PANIC.
 *
 *   修复：把整个 iretq 序列改写为独立汇编函数，避免 inline asm 的
 *   clobber 问题。RDI 全程作为基址保持不变，直到最后一条 movq 加载
 *   用户 RDI。
 *
 * 参数：RCX = struct user_regs *（用户态保存的寄存器）
 *
 * struct user_regs 布局（见 user.h）：
 *     偏移   字段     偏移   字段
 *     ----   ------   ----   ------
 *       0    r15       80    rbp
 *       8    r14       88    rdx
 *      16    r13       96    rcx
 *      24    r12      104    rbx
 *      32    r11      112    rax
 *      40    r10      120    rip
 *      48    r9       128    cs
 *      56    r8       136    rflags
 *      64    rsi      144    rsp
 *      72    rdi      152    ss
 *
 * iretq 帧顺序（从高到低入栈）：SS, RSP, RFLAGS, CS, RIP
 */

.text
.globl user_resume_asm
user_resume_asm:
    /* 保存 r 的指针到 RDI（本函数后续所有内存访问都基于 RDI） */
    movq %rcx, %rdi

    /* 关中断 + 关 SMAP 允许（AC flag = 0） */
    cli
    clac

    /* 加载用户数据段选择子 */
    movw $0x1B, %ax
    movw %ax, %ds
    movw %ax, %es

    /* push iretq 帧（逆序：SS 高地址，RIP 低地址） */
    pushq 152(%rdi)      /* ss */
    pushq 144(%rdi)      /* rsp */
    pushq 136(%rdi)      /* rflags */
    pushq 128(%rdi)      /* cs */
    pushq 120(%rdi)      /* rip */

    /* 加载通用寄存器（RDI 最后加载，因为它作为基址） */
    movq   0(%rdi), %r15
    movq   8(%rdi), %r14
    movq  16(%rdi), %r13
    movq  24(%rdi), %r12
    movq  32(%rdi), %r11
    movq  40(%rdi), %r10
    movq  48(%rdi), %r9
    movq  56(%rdi), %r8
    movq  64(%rdi), %rsi
    /* 72 = rdi，最后加载 */
    movq  80(%rdi), %rbp
    movq  88(%rdi), %rdx
    movq  96(%rdi), %rcx
    movq 104(%rdi), %rbx
    movq 112(%rdi), %rax

    /* 最后加载 RDI */
    movq  72(%rdi), %rdi

    /* iretq 回用户态 */
    iretq
/*===OmniBridgeOs/kernel/arch/x64/user/user_resume.S 结束===*/