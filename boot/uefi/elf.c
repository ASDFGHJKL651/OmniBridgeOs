#include "elf.h"

#define PT_LOAD 1

#define EI_MAG0     0
#define EI_CLASS    4
#define EI_DATA     5
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_X86_64   0x3E

EFI_STATUS elf_load(const void *image, UINTN image_size,
                    uint64_t vma_base,
                    void **out_entry, uint64_t *out_lma_base)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    if (image_size < sizeof(*eh)) return EFI_LOAD_ERROR;

    if (eh->e_ident[EI_MAG0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return EFI_LOAD_ERROR;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return EFI_LOAD_ERROR;
    if (eh->e_ident[EI_DATA]  != ELFDATA2LSB) return EFI_LOAD_ERROR;
    if (eh->e_machine != EM_X86_64) return EFI_LOAD_ERROR;
    if (eh->e_phentsize != sizeof(Elf64_Phdr)) return EFI_LOAD_ERROR;
    if (eh->e_phnum == 0) return EFI_LOAD_ERROR;

    const uint8_t *base = (const uint8_t *)image;
    uint64_t lma_base = ~0ULL;

    /* 人工必须审查：
     *   - 每个 PT_LOAD 的 p_vaddr 必须 >= vma_base（即在内核高半区）
     *   - p_paddr 由链接脚本 AT() 给出；对同一镜像，所有 PT_LOAD
     *     的 (p_paddr - (p_vaddr - vma_base)) 应恒等
     *   - p_offset + p_filesz 不得超过 image_size */
    for (int i = 0; i < eh->e_phnum; ++i) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)
            (base + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < vma_base) return EFI_LOAD_ERROR;
        if (ph->p_offset + ph->p_filesz > image_size) return EFI_LOAD_ERROR;

        /* 由该段反推镜像物理基址 */
        uint64_t rva  = ph->p_vaddr - vma_base;
        uint64_t cand = ph->p_paddr - rva;
        if (cand < lma_base) lma_base = cand;

        /* 段复制：文件内容 + BSS 清零 */
        uint8_t *dst = (uint8_t *)(uintptr_t)ph->p_paddr;
        const uint8_t *src = base + ph->p_offset;
        for (uint64_t k = 0; k < ph->p_filesz; ++k) dst[k] = src[k];
        for (uint64_t k = ph->p_filesz; k < ph->p_memsz; ++k) dst[k] = 0;
    }

    if (lma_base == ~0ULL) return EFI_LOAD_ERROR;

    *out_entry    = (void *)(uintptr_t)eh->e_entry;
    *out_lma_base = lma_base;
    return EFI_SUCCESS;
}