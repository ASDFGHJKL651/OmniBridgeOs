/*
 * 内核 x86-64 汇编入口。
 * 编译目标：x86_64-pc-win32-coff（COFF / MS ABI）
 *   - 函数参数：RCX, RDX, R8, R9
 *   - 不支持 ELF 伪指令 .type / .size
 *   - 栈 16 字节对齐
 */

.text

/* ---------------- 串口 I/O ---------------- */

.globl serial_outb
serial_outb:
    /* void serial_outb(uint16_t port, uint8_t value);
     *   rcx = port, rdx = value
     * 把 value 放到 AL，port 放到 DX，用 outb */
    mov  %rcx, %r8          /* r8 = port (临存) */
    mov  %rdx, %rax         /* rax = value（低 8 位是 AL） */
    mov  %r8, %rdx          /* rdx = port（DX 是 outb 的端口寄存器） */
    outb %al, %dx
    ret

.globl serial_inb
serial_inb:
    /* uint8_t serial_inb(uint16_t port);
     *   rcx = port
     * inb 结果在 AL，返回值按 MS ABI 从 AL 返回 */
    mov  %rcx, %rdx
    inb  %dx, %al
    ret

#ifdef OB_QEMU_EXIT
.globl qemu_exit
qemu_exit:
    /* void qemu_exit(uint32_t code);
     *   ecx = code
     * isa-debug-exit 端口 0x501 */
    mov  %ecx, %eax
    mov  $0x501, %dx
    outl %eax, %dx
    ret
#endif

/* ---------------- GDT / IDT 加载 ---------------- */

.globl gdt_flush
gdt_flush:
    /* void gdt_flush(uint64_t gdtp_addr);
     *   rcx = &gdt_ptr（MS ABI 参数 1）
     * gdt.c 中 g_gdtp 是 {uint16 limit; uint64 base;}，lgdt 直接读 */
    lgdt (%rcx)

    /* 重载数据段选择子 0x10 */
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %ax, %fs
    mov  %ax, %gs

    /* 通过 far return 重载 CS：push 目标 CS 与返回地址，再 lretq */
    pop  %rax              /* 取回被 call 压入的返回地址 */
    push $0x08             /* 内核代码段选择子 */
    push %rax
    lretq

.globl idt_load
idt_load:
    /* void idt_load(uint64_t idtp_addr);
     *   rcx = &idt_ptr */
    lidt (%rcx)
    ret

/* ---------------- 异常 stub ---------------- */

/* CPU 对无 error code 的异常只压 RFLAGS/CS/RIP，
 * 这里手动补一个 0，使栈布局与带 error code 的情况一致。
 * 之后压入 vector 号。 */
.macro ISR_NOERR num
.globl isr\num
isr\num:
    push $0
    push $\num
    jmp isr_common
.endm

/* CPU 已压入 error code，只需再压 vector 号。 */
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

/*
 * 与 idt.h 中的 struct regs 严格对应：
 *
 *   偏移   成员          来源
 *   ----   -----------   ------------------------------------
 *   0      r15           本函数 push
 *   8      r14           本函数 push
 *   ...
 *   112    rax           本函数 push
 *   120    vector        isrN 中 push
 *   128    error         CPU 或 isrN 中 push（NOERR 补 0）
 *   136    rip           CPU 压入
 *   144    cs            CPU 压入
 *   152    rflags        CPU 压入
 */
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

    /* MS ABI：第一个参数放 RCX */
    mov  %rsp, %rcx
    sub  $32, %rsp                /* shadow space（MS ABI 要求 ≥ 32 字节） */
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

    add $16, %rsp                 /* 丢弃 vector 与 error */
    iretq