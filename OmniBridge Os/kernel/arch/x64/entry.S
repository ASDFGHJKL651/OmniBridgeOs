/*===OmniBridgeOs/kernel/arch/x64/entry.S===*/
/*
 * 内核 x86-64 汇编入口与中断/系统调用桩。
 * 编译目标：x86_64-pc-win32-coff（COFF / MS ABI）
 *   - 函数参数：RCX, RDX, R8, R9
 *   - 不支持 ELF 伪指令 .type / .size
 *   - 栈 16 字节对齐
 *
 * ★ 第 18C 步修复记录（本轮）：
 *   原 syscall_entry 用 %rax 做内核栈切换的中转：
 *       movq g_syscall_kernel_rsp(%rip), %rax
 *       ...
 *       movq %rax, %rsp
 *   这会把用户态传入的 RAX（系统调用号）覆盖成内核栈顶地址。
 *   之后 push %rax 保存到 syscall_frame->rax 的就是错值，
 *   syscall_dispatcher 收到错误的 nr，落入 default 分支返回 ENOSYS。
 *
 *   修复：改用 %rsp 本身做中转（movq mem, %rsp 是合法指令），
 *   完全不再触碰 %rax。这样用户调用号被原样保存到 frame->rax。
 *
 * 说明（CR3 切换）：
 *   由于 vmm_create_address_space 现在完整复制了 PML4[0]/[256]/[511]，
 *   用户 PML4 与内核 PML4 在"内核可见内存"上一致，因此 syscall_entry
 *   与 isr_common / irq_common **无需切换 CR3**。
 */

.text

/* ============================================================
 * 串口 I/O
 * ============================================================ */

.globl serial_outb
serial_outb:
    mov  %rcx, %r8
    mov  %rdx, %rax
    mov  %r8, %rdx
    outb %al, %dx
    ret

.globl serial_inb
serial_inb:
    mov  %rcx, %rdx
    inb  %dx, %al
    ret

#ifdef OB_QEMU_EXIT
.globl qemu_exit
qemu_exit:
    mov  %ecx, %eax
    mov  $0x501, %dx
    outl %eax, %dx
    ret
#endif

/* ============================================================
 * GDT / IDT 加载
 * ============================================================ */

.globl gdt_flush
gdt_flush:
    lgdt (%rcx)

    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %ax, %fs
    mov  %ax, %gs

    pop  %rax
    push $0x08
    push %rax
    lretq

.globl idt_load
idt_load:
    lidt (%rcx)
    ret

/* ============================================================
 * 异常 stub（0..31）
 * ============================================================ */

.macro ISR_NOERR num
.globl isr\num
isr\num:
    push $0
    push $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.globl isr\num
isr\num:
    push $\num
    jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

/* ============================================================
 * IRQ stub（IRQ0..15 -> 向量 32..47）
 * ============================================================ */

.macro IRQ_STUB num
.globl irq\num
irq\num:
    push $0
    push $(32 + \num)
    jmp irq_common
.endm

IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

/* ============================================================
 * LAPIC 定时器 stub（向量 64）
 * ============================================================ */

.globl lapic_timer_isr
lapic_timer_isr:
    push $0
    push $64
    jmp irq_common

/* ============================================================
 * IPI 重调度 stub（向量 80）
 * ============================================================ */

.globl ipi_resched_isr
ipi_resched_isr:
    push $0
    push $80
    jmp irq_common

/* ============================================================
 * 异常公共入口
 * ============================================================ */

isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rdi
    push %rsi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov  %rsp, %rcx
    sub  $32, %rsp
    call exception_handler
    add  $32, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rsi
    pop %rdi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp
    iretq

/* ============================================================
 * IRQ 公共入口
 * ============================================================ */

irq_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rdi
    push %rsi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov  %rsp, %rcx
    sub  $32, %rsp
    call irq_handler
    add  $32, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rsi
    pop %rdi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp
    iretq

/* ============================================================
 * SYSCALL 进入路径
 *
 * ★★★ 第 18C 步核心修复（本轮）★★★
 *
 *   原实现：
 *       movq g_syscall_kernel_rsp(%rip), %rax
 *       testq %rax, %rax
 *       jnz   0f
 *       leaq  g_syscall_kstack_top(%rip), %rax
 *     0: movq %rax, %rsp
 *
 *   问题：用 %rax 做中转，覆盖了用户态传入的系统调用号
 *   （syscall 指令约定 nr 放在 RAX 中）。之后 push %rax 保存
 *   到 syscall_frame->rax 的就是内核栈顶地址而非调用号，
 *   导致 syscall_dispatcher 收到错误 nr，全部落入 default 分支
 *   返回 -ENOSYS。
 *
 *   修复：直接用 %rsp 做中转。
 *     - movq mem, %rsp 是合法的（x86-64 允许直接加载到 RSP）
 *     - testq %rsp, %rsp 检查是否为 0
 *     - 若为 0，leaq 加载静态后备栈顶到 %rsp
 *   全程不触碰 %rax，用户 nr 被原样保留。
 *
 *   栈布局（push 顺序从先到后：rax, rbx, rcx, rdx, rsi, rdi,
 *   rbp, r8, r9, r10, r11, r12, r13, r14, r15）：
 *     低地址 → 高地址：
 *       offset 0   : r15
 *       offset 8   : r14
 *       ...
 *       offset 96  : rcx   (用户返回 RIP)
 *       offset 104 : rbx
 *       offset 112 : rax   (系统调用号 / 返回值)
 *       offset 120 : (未使用；本入口未保存用户 CR3)
 *       offset 128 : r11   (用户 RFLAGS)
 *
 *   syscall_frame 结构与上述完全一致，f->rax 位于 RSP + 112。
 * ============================================================ */

.globl syscall_entry
syscall_entry:
    swapgs

    /* ---- 1) 保存用户 RSP 到全局临时变量 ----
     *
     * 说明：这一句之后到下面 pushq g_syscall_user_rsp 之间的几条
     * 指令不会被抢占（IF=0），因此全局变量在这里充当一个"寄存器"
     * 是安全的；一旦压入内核栈，就永远不再依赖它。
     */
    movq %rsp, g_syscall_user_rsp(%rip)

    /* ---- 2) 切换到内核栈（不触碰 RAX！） ---- */
    movq g_syscall_kernel_rsp(%rip), %rsp
    testq %rsp, %rsp
    jnz   0f
    leaq  g_syscall_kstack_top(%rip), %rsp
0:

    /* ---- 3) 立即把用户 RSP 压入**本线程内核栈** ----
     *
     * ★★★ 第 18C 步关键修复 ★★★
     *
     * g_syscall_user_rsp 是全局变量，多用户线程并发 syscall 会互相
     * 覆盖。若本线程在 syscall 中被抢占（例如 ThreadWait 中的 hlt
     * 循环），其他线程进入 syscall 会覆盖该变量；本线程稍后 sysretq
     * 前读该变量会拿到错误值，导致：
     *   - RSP 串号（1001 用 1002 的用户栈）
     *   - 用户态 return 弹出错误地址
     *   - SMEP 拦截内核态执行用户地址 → #GP
     *   - RIP=0x40097 CS=0x8 ERR=0（症状）
     *
     * 修复：把 user_rsp 立即压入内核栈。内核栈 per-task 独立，无论
     * 多少次抢占都不会被其他线程污染。
     *
     * 栈平衡：此 pushq 与 sysretq 之前的 popq %rsp 对称。
     */
    pushq g_syscall_user_rsp(%rip)

    /* ---- 4) 诊断：向 COM1 写 'S' ---- */
    pushq %rax
    pushq %rdx
    movw  $0x3F8, %dx
    movb  $0x53, %al
    outb  %al, %dx
    popq  %rdx
    popq  %rax

    /* ---- 5) 条件 STAC：仅当 CR4.SMAP=1 ---- */
    push %rax
    mov  %cr4, %rax
    test $0x200000, %rax
    jz   1f
    stac
1:  pop  %rax

    /* ---- 6) 保存 15 个通用寄存器到 syscall_frame ---- */
    push %rax       /* offset 112：系统调用号 / 返回值 */
    push %rbx
    push %rcx       /* offset 96 ：用户返回 RIP */
    push %rdx       /* offset 88 ：arg2 */
    push %rsi       /* offset 80 ：arg1 */
    push %rdi       /* offset 72 ：arg0 */
    push %rbp
    push %r8
    push %r9
    push %r10       /* offset 40 ：arg3 */
    push %r11       /* offset 32 ：用户 RFLAGS */
    push %r12
    push %r13
    push %r14
    push %r15       /* offset 0  */

    /* MS ABI：rcx = &syscall_frame */
    mov  %rsp, %rcx

    /* 15 个 qword = 120 字节，sub 8 使栈 16 字节对齐；
     * 再 sub 32 作为 MS ABI shadow space。 */
    sub  $8, %rsp
    sub  $32, %rsp
    call syscall_dispatcher
    add  $32, %rsp
    add  $8, %rsp

#ifdef OB_SYSCALL_DEBUG_HLT
    hlt
#endif

    /* ---- 7) 把返回值写回栈上 RAX 槽位（RSP + 112） ---- */
    mov  %rax, 112(%rsp)

    /* ---- 8) 恢复 15 个通用寄存器 ---- */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx        /* 恢复用户返回 RIP */
    pop %rbx
    pop %rax        /* 恢复返回值到 RAX，sysretq 会带回到用户态 */

    /* ---- 9) 条件 CLAC ---- */
    push %rax
    mov  %cr4, %rax
    test $0x200000, %rax
    jz   2f
    clac
2:  pop  %rax

    /* ---- 10) 从**本线程内核栈**弹出用户 RSP ----
     *
     * ★ 关键修复：不再读 g_syscall_user_rsp！
     *   此处弹出的 user_rsp 是进入 syscall 时（步骤 3）压入的，
     *   全程由本线程独占，不受其他线程 syscall 影响。
     */
    popq %rsp

    swapgs
    sysretq

/* ============================================================
 * 全局变量：syscall 栈切换状态（由 user.c 写入）
 * ============================================================ */

.section .bss
.align 16
.globl g_syscall_kernel_rsp
g_syscall_kernel_rsp:
    .quad 0

.align 16
.globl g_syscall_user_rsp
g_syscall_user_rsp:
    .quad 0

/* 静态后备内核栈（16 KB）：若 user.c 未设置 g_syscall_kernel_rsp，
 * syscall_entry 使用此栈。位于 .bss，每个进程共享——18C 只有单个
 * 用户态进程时安全；多用户态进程时需改为 per-task。 */
.align 16
g_syscall_kstack:
    .skip 16384
.globl g_syscall_kstack_top
g_syscall_kstack_top:
/*===OmniBridgeOs/kernel/arch/x64/entry.S 结束===*/