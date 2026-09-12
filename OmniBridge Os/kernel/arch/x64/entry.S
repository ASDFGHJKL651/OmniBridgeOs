/*
 * 内核 x86-64 汇编入口与中断/系统调用桩。
 * 编译目标：x86_64-pc-win32-coff（COFF / MS ABI）
 *   - 函数参数：RCX, RDX, R8, R9
 *   - 不支持 ELF 伪指令 .type / .size
 *   - 栈 16 字节对齐
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
 * SYSCALL 进入路径（骨架）
 *
 * 进入时（由 SYSCALL 指令硬件设置）：
 *   RCX = 用户态返回 RIP
 *   R11 = 用户态 RFLAGS
 *   CS/SS 由 STAR[47:32] 指定（内核 CS=0x08 / SS=0x10）
 *   RSP 仍是用户态 RSP（当前无用户态，等价于内核态栈）
 *
 * 保存顺序（push 从高地址往低地址）：
 *   rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10,
 *   r11, r12, r13, r14, r15
 *
 * 因此在栈上从低地址到高地址依次为：
 *   r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi,
 *   rdx, rcx, rbx, rax
 *
 * 这与 struct syscall_frame 的字段顺序必须严格一致。
 * ============================================================ */

.globl syscall_entry
syscall_entry:
    swapgs

    push %rax
    push %rbx
    push %rcx                         /* 用户返回 RIP */
    push %rdx
    push %rsi                         /* arg1 */
    push %rdi                         /* arg0 */
    push %rbp
    push %r8
    push %r9
    push %r10                         /* arg3 */
    push %r11                         /* 用户 RFLAGS */
    push %r12
    push %r13
    push %r14
    push %r15

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
    /* 调试模式：只验证入口被调用与参数打印，不执行 SYSRET。
     * 避免因当前 GDT 布局下 SYSRET 目标 CS 异常（0x2B，指向 TSS
     * 描述符），导致紧随其后的取指触发 #PF 而干扰观察。 */
    hlt
#endif

    /* 正常路径：恢复通用寄存器后返回 */
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
    pop %rcx
    pop %rbx
    pop %rax

    swapgs
    sysretq