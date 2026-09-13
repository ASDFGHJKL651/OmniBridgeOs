/*
 * UEFI 引导器 -> 内核入口 跳转桩。
 * 编译目标：x86_64-pc-win32-coff（COFF / MS ABI）
 * UEFI 与内核都使用 MS ABI：
 *   rcx = boot_info, rdx = entry_va（进入内核后仍有效）
 * 只做栈对齐，然后 jmp。
 */

.text
.globl uefi_jump_to_kernel

uefi_jump_to_kernel:
    and  $-16, %rsp          /* 栈 16 字节对齐 */
    jmp  *%rdx               /* 跳转到内核 VMA，不返回 */