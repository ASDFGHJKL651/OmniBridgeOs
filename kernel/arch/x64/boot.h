/*===OmniBridgeOs/kernel/arch/x64/boot.h===*/
#ifndef OMNIBRIDGE_BOOT_H
#define OMNIBRIDGE_BOOT_H

#include <stdint.h>

#define OB_MAX_MEMORY_MAP_ENTRIES 256

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

    /* ★ 第 18C 步修复：内核镜像实际占用的物理范围。
     *
     * 人工必须审查：
     *   - UEFI 内存图可能仍把内核加载区标记为 EfiBootServicesCode/Data，
     *     而 pmm_init() 会将这些类型视为"可用"。
     *   - 若不显式排除该范围，PMM 会把内核镜像/内核 BSS 所在的物理页
     *     加入 free_lists，导致用户程序的 malloc/brk 覆写内核 BSS，
     *     引发随机崩溃（表现为 "free_list[..] head ... not PG_FREE"）。
     *
     *   - kernel_phys_start = OB_KERNEL_LMA
     *   - kernel_phys_end   = 所有 PT_LOAD 段 p_paddr + p_memsz 的最大值
     *
     *   旧 UEFI 若未填充这两个字段（均为 0），pmm_init 使用保守默认值
     *   [0x200000, 0x400000)。 */
    uint64_t kernel_phys_start;
    uint64_t kernel_phys_end;

    struct ob_memory_entry memory_map[OB_MAX_MEMORY_MAP_ENTRIES];
};

void _kstart_c(struct ob_boot_info *boot_info);

#endif
/*===OmniBridgeOs/kernel/arch/x64/boot.h 结束===*/