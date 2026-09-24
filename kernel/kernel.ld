/*
 * OmniBridge 内核链接脚本。
 *
 * 与 kernel/arch/x64/boot.h 中的宏保持一致：
 *   OB_KERNEL_LMA = 0x0000000000200000
 *   OB_KERNEL_VMA = 0xFFFFFFFF80000000
 *
 * 段边界符号导出（人工必须审查）：
 *   __text_start / __text_end     -> vmm.c 映射为 RX
 *   __rodata_start / __rodata_end -> vmm.c 映射为 R
 *   __data_start / __data_end     -> vmm.c 映射为 RW (NX)
 *   __bss_start / __bss_end       -> vmm.c 映射为 RW (NX)，并清零
 *
 * 注意：段首地址通过 ALIGN(4096) 保证与 4K 页边界对齐，
 *       避免 vmm_init() 中按段设置页权限时跨页冲突。
 *       .text 保证 2MB 对齐（起始 LMA 0x200000 本身即 2MB 对齐），
 *       其余段只要求 4K 对齐。
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x00200000;   /* 2 MB，必须 2MB 对齐 */

SECTIONS
{
    . = KERNEL_VMA;

    .text ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.text) - KERNEL_VMA)) {
        __text_start = .;
        *(.text.kernel_entry)
        *(.text*)
        __text_end = .;
    }

    .rodata ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.rodata) - KERNEL_VMA)) {
        __rodata_start = .;
        *(.rodata*)
        __rodata_end = .;
    }

    .data ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.data) - KERNEL_VMA)) {
        __data_start = .;
        *(.data*)
        __data_end = .;
    }

    .bss ALIGN(4096) : AT(KERNEL_LMA + (ADDR(.bss) - KERNEL_VMA)) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) *(.rela*) }
}