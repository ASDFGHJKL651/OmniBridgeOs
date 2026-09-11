#ifndef OMNIBRIDGE_IDT_H
#define OMNIBRIDGE_IDT_H

#include <stdint.h>

struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags;
};

void idt_init(void);
void exception_handler(struct regs *r);

#endif