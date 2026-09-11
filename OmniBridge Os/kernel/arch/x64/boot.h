#ifndef OMNIBRIDGE_BOOT_H
#define OMNIBRIDGE_BOOT_H

#include <stdint.h>

#define OB_MAX_MEMORY_MAP_ENTRIES 256

/* 内核物理加载基址与虚拟链接基址：必须与 kernel.ld、vmm.c、
 * boot/uefi/page.c 保持一致。
 *   - LMA 必须 2MB 对齐（2MB 大页映射）
 *   - VMA 必须是 0xFFFFFFFF80000000（与 PML4[511]/PDPT[510] 对应） */
#define OB_KERNEL_LMA 0x0000000000200000ULL
#define OB_KERNEL_VMA 0xFFFFFFFF80000000ULL

struct ob_memory_entry {
    uint32_t type;
    uint32_t _pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

struct ob_boot_info {
    uint64_t memory_map_size;
    uint64_t descriptor_size;
    uint32_t descriptor_version;
    uint32_t entry_count;
    struct ob_memory_entry memory_map[OB_MAX_MEMORY_MAP_ENTRIES];
};

/* 内核 C 入口（由 kernel/arch/x64/entry_kernel.S 调用） */
void _kstart_c(struct ob_boot_info *boot_info);

#endif