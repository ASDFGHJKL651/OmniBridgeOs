#include "page.h"

static uint64_t pml4[512]     __attribute__((aligned(4096)));
static uint64_t pdpt_lo[512]  __attribute__((aligned(4096)));
static uint64_t pd_lo[4][512] __attribute__((aligned(4096)));
static uint64_t pdpt_dm[512]  __attribute__((aligned(4096)));
static uint64_t pd_dm[4][512] __attribute__((aligned(4096)));
static uint64_t pdpt_k[512]   __attribute__((aligned(4096)));
static uint64_t pd_k[512]     __attribute__((aligned(4096)));

#define PTE_P  1ULL
#define PTE_RW (1ULL << 1)
#define PTE_PS (1ULL << 7)
#define PTE_NX (1ULL << 63)

static void zero(uint64_t *p)
{
    for (int i = 0; i < 512; ++i) p[i] = 0;
}

/*
 * 人工必须审查：
 *   - 只构造页表，不切换 CR3。切换由 main.c 在 ExitBootServices 之后执行。
 *   - kernel_lma 必须 2MB 对齐。
 *   - 内核镜像区域必须是 RWH（可执行），不能带 NX，否则取指立即 #PF。
 *   - UEFI 环境为恒等映射，因此 PML4/PDPT/PD 的"虚拟地址"就是"物理地址"，
 *     直接把数组地址写入页表项即可。
 */
void uefi_build_page_tables(uint64_t kernel_lma, void **out_pml4_phys)
{
    zero(pml4); zero(pdpt_lo); zero(pdpt_dm); zero(pdpt_k);
    for (int i = 0; i < 4; ++i) { zero(pd_lo[i]); zero(pd_dm[i]); }
    zero(pd_k);

    const uint64_t RWH  = PTE_P | PTE_RW | PTE_PS;         /* 可执行 */
    const uint64_t RWHN = RWH | PTE_NX;                    /* 不可执行 */

    /* 1) 恒等映射 0..4GB（过渡用，必须可执行，因为从 UEFI 代码跳到内核代码
     *    的瞬间 CPU 还在用恒等映射下的 UEFI 段） */
    pml4[0] = ((uint64_t)pdpt_lo) | PTE_P | PTE_RW;
    for (int gb = 0; gb < 4; ++gb) {
        pdpt_lo[gb] = ((uint64_t)pd_lo[gb]) | PTE_P | PTE_RW;
        for (int i = 0; i < 512; ++i)
            pd_lo[gb][i] = (((uint64_t)gb << 30) | ((uint64_t)i << 21)) | RWH;
    }

    /* 2) DirectMap 0xFFFF800000000000 + phys（数据区，NX 正确） */
    pml4[256] = ((uint64_t)pdpt_dm) | PTE_P | PTE_RW;
    for (int gb = 0; gb < 4; ++gb) {
        pdpt_dm[gb] = ((uint64_t)pd_dm[gb]) | PTE_P | PTE_RW;
        for (int i = 0; i < 512; ++i)
            pd_dm[gb][i] = (((uint64_t)gb << 30) | ((uint64_t)i << 21)) | RWHN;
    }

    /* 3) 内核高半区镜像 0xFFFFFFFF80000000 -> 物理 kernel_lma
     *    ★ 必须是 RWH（可执行），否则 CPU 一取指就 #PF（I=1, P=1） */
    pml4[511] = ((uint64_t)pdpt_k) | PTE_P | PTE_RW;
    pdpt_k[510] = ((uint64_t)pd_k) | PTE_P | PTE_RW;
    for (int i = 0; i < 512; ++i)
        pd_k[i] = (kernel_lma + ((uint64_t)i << 21)) | RWH;

    /*
     * ★ 不再在此切换 CR3。
     *   调用者（main.c）会在 ExitBootServices 之后、跳转内核之前，
     *   用 out_pml4_phys 中的值执行 `mov %cr3`。
     *   这避免了 OVMF 内部的异步事件/驱动回调（例如 VirtIO 网卡 UEFI
     *   驱动）在错误的页表下运行导致的 #PF。
     *
     *   注：UEFI 环境是恒等映射，pml4 数组的虚拟地址 = 物理地址。
     */
    if (out_pml4_phys) {
        *out_pml4_phys = (void *)pml4;
    }
}