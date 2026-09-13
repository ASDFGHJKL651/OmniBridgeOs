/*
 * UEFI 引导器专用 I/O 封装（COFF 目标）。
 * MS ABI：参数 1 = RCX，参数 2 = RDX。
 */

.text

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