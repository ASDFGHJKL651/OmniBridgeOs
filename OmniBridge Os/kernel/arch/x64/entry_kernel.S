/*
 * 内核入口。
 * 编译目标：x86_64-pc-win32-coff（COFF / MS ABI）
 * 进入时：
 *   - 运行在 UEFI 建的初始 CR3 下
 *   - RCX = &boot_info （MS ABI 参数 1）
 *   - 栈仍是 UEFI 栈
 */

.section .bss
.align 16
kernel_stack_bottom:
    .skip 65536
kernel_stack_top:

.section .text
.globl kernel_entry

kernel_entry:
    cli
    lea    kernel_stack_top(%rip), %rsp
    xor    %rbp, %rbp
    /* RCX 保持为 &boot_info，直接调用 */
    call   _kstart_c
1:  hlt
    jmp    1b