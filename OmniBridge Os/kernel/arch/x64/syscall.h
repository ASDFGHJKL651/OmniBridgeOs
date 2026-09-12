#ifndef OMNIBRIDGE_SYSCALL_H
#define OMNIBRIDGE_SYSCALL_H

#include <stdint.h>

/*
 * SYSCALL 进入时的寄存器现场。
 *
 * 必须与 entry.S 中 syscall_entry 的 push 顺序严格一致。
 * 栈布局（从低地址到高地址，也即 offset 0 起）：
 *
 *   offset  0  : r15
 *   offset  8  : r14
 *   offset 16  : r13
 *   offset 24  : r12
 *   offset 32  : r11   （SYSCALL 硬件写入的用户 RFLAGS）
 *   offset 40  : r10   （arg3）
 *   offset 48  : r9
 *   offset 56  : r8
 *   offset 64  : rbp
 *   offset 72  : rdi   （arg0）
 *   offset 80  : rsi   （arg1）
 *   offset 88  : rdx   （arg2）
 *   offset 96  : rcx   （SYSCALL 硬件写入的用户返回 RIP）
 *   offset 104 : rbx
 *   offset 112 : rax   （系统调用号）
 *
 * 注意：rdi/rsi/rbp 的顺序必须与汇编 push 顺序一致——先 push rsi、
 * 再 push rdi、最后 push rbp，因此栈上从低到高是 rbp、rdi、rsi。
 */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
};

/*
 * 初始化 SYSCALL/SYSRET：
 *   - EFER.SCE = 1
 *   - STAR：内核 CS=0x08 / SS=0x10，用户基准 0x1B（当前 GDT 占位值）
 *   - LSTAR：syscall_entry 地址
 *   - FMASK：进入时清 IF/TF/DF/NT/AC
 */
void syscall_init(void);

/*
 * syscall 骨架分发器：
 *   - 打印系统调用号与参数
 *   - 返回 -ENOSYS（-38）
 * 真正的 syscall_dispatcher 在后续步骤中实现。
 */
int64_t syscall_dispatcher(struct syscall_frame *f);

#endif