#include "vmm.h"
#include "printk.h"
#include "pmm.h"
#include "boot.h"          /* OB_KERNEL_LMA / OB_KERNEL_VMA */

/* 内核 VMA -> 物理地址。内核页表是内核 BSS 中的静态数组，
 * 其虚拟地址位于 0xFFFFFFFF80000000 起的高半区，物理地址 = VMA - VMA_BASE + LMA_BASE。
 * 页表条目和 CR3 都必须使用物理地址。 */
#define VMA2PA(x) (((uint64_t)(x) - OB_KERNEL_VMA) + OB_KERNEL_LMA)

static uint64_t g_pml4       [512] __attribute__((aligned(4096)));
static uint64_t g_pdpt_low   [512] __attribute__((aligned(4096)));
static uint64_t g_pd_low     [4][512] __attribute__((aligned(4096)));
static uint64_t g_pdpt_direct[512] __attribute__((aligned(4096)));
static uint64_t g_pd_direct  [4][512] __attribute__((aligned(4096)));
static uint64_t g_pdpt_kern  [512] __attribute__((aligned(4096)));
static uint64_t g_pd_kern    [512] __attribute__((aligned(4096)));

static inline void zero_page(void *p)
{
    uint64_t *q = (uint64_t *)p;
    for (int i = 0; i < 512; ++i) q[i] = 0;
}

static void fill_1gb(uint64_t pd[512], uint64_t phys_base, uint64_t flags)
{
    for (int i = 0; i < 512; ++i) {
        pd[i] = (phys_base + ((uint64_t)i << 21)) | flags;
    }
}

static void load_cr3(uint64_t pml4_phys)
{
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

void vmm_init(void)
{
    zero_page(g_pml4);
    zero_page(g_pdpt_low);
    zero_page(g_pdpt_direct);
    zero_page(g_pdpt_kern);
    for (int i = 0; i < 4; ++i) {
        zero_page(g_pd_low[i]);
        zero_page(g_pd_direct[i]);
    }
    zero_page(g_pd_kern);

    const uint64_t RW   = PTE_PRESENT | PTE_WRITABLE;
    const uint64_t RWH  = RW | PTE_HUGE;              /* 可执行 */
    const uint64_t RWHN = RWH | PTE_NX;               /* 不可执行 */

    /* 1) 低恒等映射 0..4GB
     *    必须可执行：UEFI 到内核代码切换的第一瞬间，CPU 用恒等映射下的 UEFI 栈；
     *    同时 pmm/kmalloc 目前直接把物理地址当指针使用，依赖恒等映射可读写。 */
    g_pml4[0] = VMA2PA(g_pdpt_low) | RW;
    for (int gb = 0; gb < 4; ++gb) {
        g_pdpt_low[gb] = VMA2PA(g_pd_low[gb]) | RW;
        fill_1gb(g_pd_low[gb], (uint64_t)gb << 30, RWH);
    }

    /* 2) DirectMap 0xFFFF800000000000 + phys（数据区，NX） */
    g_pml4[256] = VMA2PA(g_pdpt_direct) | RW;
    for (int gb = 0; gb < 4; ++gb) {
        g_pdpt_direct[gb] = VMA2PA(g_pd_direct[gb]) | RW;
        fill_1gb(g_pd_direct[gb], (uint64_t)gb << 30, RWHN);
    }

    /* 3) 内核高半区 0xFFFFFFFF80000000 起 1GB，物理基址 = OB_KERNEL_LMA
     *    必须可执行（RWH），否则内核第一条指令取指即触发 #PF（I=1, P=1）。 */
    g_pml4[511] = VMA2PA(g_pdpt_kern) | RW;
    g_pdpt_kern[510] = VMA2PA(g_pd_kern) | RW;
    fill_1gb(g_pd_kern, OB_KERNEL_LMA, RWH);

    /* ★ 关键：CR3 必须是 PML4 的物理地址 */
    load_cr3(VMA2PA(g_pml4));
    printk("[VMM] 4-level paging enabled\n");
}

uint64_t *vmm_current_pml4(void)
{
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(cr3 & PTE_ADDR_MASK);
}

int vmm_map_2mb(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t i4 = PML4_INDEX(virt);
    uint64_t i3 = PDPT_INDEX(virt);
    uint64_t i2 = PD_INDEX(virt);

    if (!(pml4[i4] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)(pml4[i4] & PTE_ADDR_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return -1;
    uint64_t *pd = (uint64_t *)(pdpt[i3] & PTE_ADDR_MASK);
    if (pd[i2] & PTE_PRESENT) return -2;

    pd[i2] = (phys & ~0x1FFFFFULL) | flags;
    return 0;
}