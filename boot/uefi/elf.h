#ifndef OMNIBRIDGE_UEFI_ELF_H
#define OMNIBRIDGE_UEFI_ELF_H

#include <stdint.h>
#include "uefi_min.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

/* 校验 ELF 头并把 PT_LOAD 段按 p_paddr 复制到物理内存，BSS 清零。
 *
 *   vma_base  —— 镜像的虚拟基址（例如 OB_KERNEL_VMA）
 *   out_entry —— e_entry（虚拟地址）
 *   out_lma_base —— 镜像物理基址 = p_paddr - (p_vaddr - vma_base)
 *                   对于所有 PT_LOAD 应恒等；任一 PT_LOAD 都能推出。
 */
EFI_STATUS elf_load(const void *image, UINTN image_size,
                    uint64_t vma_base,
                    void **out_entry, uint64_t *out_lma_base);

#endif