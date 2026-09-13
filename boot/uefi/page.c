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

/* 人工必须审查：
 *   - 加载新 CR3 后，当前 RIP 与 RSP 必须仍在映射中（恒等映射覆盖 UEFI 镜像物理地址）
 *   - kernel_lma 必须 2MB 对齐
 *   - 内核镜像区域必须是 RWH（可执行），不能带 NX，否则取指立即 #PF */
void uefi_build_page_tables(uint64_t kernel_lma)
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

    __asm__ __volatile__("mov %0, %%cr3" :: "r"((uint64_t)pml4) : "memory");
}