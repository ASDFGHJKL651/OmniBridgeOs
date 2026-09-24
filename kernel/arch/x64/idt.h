#ifndef OMNIBRIDGE_IDT_H
#define OMNIBRIDGE_IDT_H

#include <stdint.h>

/*
 * 中断/异常现场。必须与 entry.S 中 isr_common / irq_common 的压栈顺序严格一致：
 *
 *   偏移   成员      来源
 *   ----   --------  --------------------------------------
 *   0      r15       isr_common/irq_common 中 push
 *   8      r14       ...
 *   ...
 *   112    rax       ...
 *   120    vector    isrN/irqN 中 push
 *   128    error     CPU 或 isrN 中 push（NOERR 补 0）
 *   136    rip       CPU 压入
 *   144    cs        CPU 压入
 *   152    rflags    CPU 压入
 *   （若发生特权级切换，ss、rsp 会被 CPU 额外压入，但不影响本结构）
 */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags;
};

/* 初始化 IDT：注册 0-31 异常门 + 32-47 IRQ 门 */
void idt_init(void);

/* 异常处理（isr_common 调用），不返回 */
void exception_handler(struct regs *r);

/* 页错误处理（vector 14）。由 exception_handler 分派。不返回。 */
void page_fault_handler(struct regs *r);

/* IRQ 处理（irq_common 调用），返回后由汇编 iretq */
void irq_handler(struct regs *r);

#endif