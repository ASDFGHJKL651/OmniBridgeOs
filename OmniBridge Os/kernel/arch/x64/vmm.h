#ifndef OMNIBRIDGE_VMM_H
#define OMNIBRIDGE_VMM_H

#include <stdint.h>

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_WRITETHRU (1ULL << 3)
#define PTE_NOCACHE   (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_INDEX(v) (((uint64_t)(v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((uint64_t)(v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((uint64_t)(v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((uint64_t)(v) >> 12) & 0x1FF)

void vmm_init(void);

/* 在给定 PML4 中映射一个 2MB 大页 */
int  vmm_map_2mb(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/* 当前 CR3 指向的 PML4 */
uint64_t *vmm_current_pml4(void);

#endif