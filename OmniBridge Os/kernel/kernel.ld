OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

/* 与 kernel/arch/x64/boot.h 中的宏保持一致：
 *   OB_KERNEL_LMA = 0x0000000000200000
 *   OB_KERNEL_VMA = 0xFFFFFFFF80000000 */
KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x00200000;   /* 2 MB，必须 2MB 对齐 */

SECTIONS
{
    . = KERNEL_VMA;

    .text ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.text) - KERNEL_VMA)) {
        *(.text.kernel_entry)
        *(.text*)
    }

    .rodata ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.rodata) - KERNEL_VMA)) {
        *(.rodata*)
    }

    .data ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.data) - KERNEL_VMA)) {
        *(.data*)
    }

    .bss ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.bss) - KERNEL_VMA)) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) *(.rela*) }
}