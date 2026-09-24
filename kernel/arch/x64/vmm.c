/*===OmniBridgeOs/kernel/arch/x64/vmm.c===*/
#include "vmm.h"
#include "printk.h"
#include "pmm.h"
#include "boot.h"
#include "serial.h"

#define VMA2PA(x) (((uint64_t)(x) - OB_KERNEL_VMA) + OB_KERNEL_LMA)

#define PMM_PHYS_LOW_LIMIT   0x100000ULL
#define PMM_PHYS_HIGH_LIMIT  ((uint64_t)MAX_PAGES << PAGE_SHIFT)
#define PT_BITMAP_BYTES      ((MAX_PAGES + 7) / 8)

static uint8_t  g_pt_bitmap[PT_BITMAP_BYTES];
static uint32_t g_pt_owner[MAX_PAGES];

static uint64_t g_pml4       [512] __attribute__((aligned(4096)));
static uint64_t g_pdpt_low   [512] __attribute__((aligned(4096)));
static uint64_t g_pd_low     [4][512] __attribute__((aligned(4096)));
static uint64_t g_pdpt_direct[512] __attribute__((aligned(4096)));
static uint64_t g_pd_direct  [4][512] __attribute__((aligned(4096)));
static uint64_t g_pdpt_kern  [512] __attribute__((aligned(4096)));
static uint64_t g_pd_kern    [512] __attribute__((aligned(4096)));

static int g_smep_enabled = 0;
static int g_smap_enabled = 0;

static uint64_t *g_kernel_pml4 = 0;

static inline void zero_page(void *p)
{
    uint64_t *q = (uint64_t *)p;
    for (int i = 0; i < 512; ++i) q[i] = 0;
}

/* -------- 页表页身份位图操作 -------- */
static inline int pt_bit_test(uint64_t pa)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if (pfn >= MAX_PAGES) return 0;
    return (g_pt_bitmap[pfn >> 3] >> (pfn & 7)) & 1;
}

static inline void pt_bit_set(uint64_t pa)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if (pfn >= MAX_PAGES) return;
    g_pt_bitmap[pfn >> 3] |= (uint8_t)(1u << (pfn & 7));
}

static inline void pt_bit_clear(uint64_t pa)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if (pfn >= MAX_PAGES) return;
    g_pt_bitmap[pfn >> 3] &= (uint8_t)~(1u << (pfn & 7));
}

/* -------- 页表页归属标签操作 -------- */
static inline uint32_t pt_owner_get(uint64_t pa)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if (pfn >= MAX_PAGES) return 0xFFFFFFFFu;
    return g_pt_owner[pfn];
}

static inline void pt_owner_set(uint64_t pa, uint32_t tag)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if (pfn >= MAX_PAGES) return;
    g_pt_owner[pfn] = tag;
}

/*
 * ★★★ 第 18C 步修复（本轮）：新增关键判定函数 ★★★
 *
 * vmm_is_pt_page_strict —— 严格判定 pa 是否为 vmm 分配的页表页。
 *
 * 与 vmm_is_pt_page 的区别：
 *   - vmm_is_pt_page 只检查 g_pt_bitmap；
 *   - vmm_is_pt_page_strict 同时要求 struct page 的 flags 不是
 *     PG_RESERVED，也就是该页确实由 pmm 分配过。
 *
 * 用于在释放路径上拒绝"位图损坏"或"页已被错误 free 到 free_lists"
 * 的危险情况。
 */
static int vmm_is_pt_page_strict(uint64_t pa)
{
    if (pa & 0xFFFULL) return 0;
    if (pa < PMM_PHYS_LOW_LIMIT) return 0;
    if (pa >= PMM_PHYS_HIGH_LIMIT) return 0;
    if (!pt_bit_test(pa)) return 0;

    /* ★ 额外校验：页必须是 pmm 分配过的（不是 PG_RESERVED） */
    struct page *pg = phys_to_page(pa);
    if (pg->flags & PG_RESERVED) return 0;
    return 1;
}

static struct page *vmm_alloc_pt_page(void)
{
    struct page *pg = pmm_alloc_pages(0);
    if (!pg) return 0;

    uint64_t pa = page_to_phys(pg);
    if (pa & 0xFFFULL) { pmm_free_pages(pg, 0); return 0; }
    if (pa < PMM_PHYS_LOW_LIMIT) { pmm_free_pages(pg, 0); return 0; }
    if (pa >= PMM_PHYS_HIGH_LIMIT) { pmm_free_pages(pg, 0); return 0; }

    /* ★ 关键：清零整页，杜绝陈旧数据 */
    zero_page((void *)(DIRECTMAP_BASE + pa));

    pt_owner_set(pa, 0);
    pt_bit_set(pa);
    return pg;
}

static int vmm_free_pt_page_ex(uint64_t pa, uint32_t owner)
{
    if (pa & 0xFFFULL) return -1;
    if (pa < PMM_PHYS_LOW_LIMIT) return -1;
    if (pa >= PMM_PHYS_HIGH_LIMIT) return -1;

    /* ★ 本轮加强：使用 strict 判定 */
    if (!vmm_is_pt_page_strict(pa)) {
        printk("[VMM] WARN: pa=0x%llx not a valid PT page (strict), skip\n",
               (unsigned long long)pa);
        return -1;
    }

    if (owner != 0xFFFFFFFFu) {
        uint32_t actual = pt_owner_get(pa);
        if (actual != owner) {
            printk("[VMM] WARN: pt pa=0x%llx owner=%u != %u, skip\n",
                   (unsigned long long)pa, (unsigned)actual,
                   (unsigned)owner);
            return -1;
        }
    }

    struct page *pg = phys_to_page(pa);
    if (pg->flags & PG_RESERVED) {
        printk("[VMM] WARN: PT page pa=0x%llx is RESERVED, skip\n",
               (unsigned long long)pa);
        pt_bit_clear(pa);
        pt_owner_set(pa, 0);
        return -1;
    }
    if (pg->flags & PG_SLAB) {
        printk("[VMM] WARN: PT page pa=0x%llx is SLAB, skip\n",
               (unsigned long long)pa);
        pt_bit_clear(pa);
        pt_owner_set(pa, 0);
        return -1;
    }
    if (pg->flags & PG_FREE) {
        printk("[VMM] WARN: PT page pa=0x%llx is already FREE, skip\n",
               (unsigned long long)pa);
        pt_bit_clear(pa);
        pt_owner_set(pa, 0);
        return -1;
    }

    zero_page((void *)(DIRECTMAP_BASE + pa));

    pt_bit_clear(pa);
    pt_owner_set(pa, 0);
    pmm_free_pages(pg, 0);
    return 0;
}

static inline uint64_t read_cr4(void)
{
    uint64_t v;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v)
{
    __asm__ __volatile__("mov %0, %%cr4" :: "r"(v) : "memory");
}

static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                         : "a"(leaf), "c"(subleaf));
}

void vmm_init(void)
{
    for (uint64_t i = 0; i < PT_BITMAP_BYTES; ++i) g_pt_bitmap[i] = 0;
    for (uint64_t i = 0; i < MAX_PAGES; ++i) g_pt_owner[i] = 0;

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
    const uint64_t RWH  = RW | PTE_HUGE;
    const uint64_t RWHN = RWH | PTE_NX;

    g_pml4[0] = VMA2PA(g_pdpt_low) | RW;
    for (int gb = 0; gb < 4; ++gb) {
        g_pdpt_low[gb] = VMA2PA(g_pd_low[gb]) | RW;
        for (int i = 0; i < 512; ++i) {
            g_pd_low[gb][i] =
                (((uint64_t)gb << 30) | ((uint64_t)i << 21)) | RWH;
        }
    }

    g_pml4[256] = VMA2PA(g_pdpt_direct) | RW;
    for (int gb = 0; gb < 4; ++gb) {
        g_pdpt_direct[gb] = VMA2PA(g_pd_direct[gb]) | RW;
        for (int i = 0; i < 512; ++i) {
            g_pd_direct[gb][i] =
                (((uint64_t)gb << 30) | ((uint64_t)i << 21)) | RWHN;
        }
    }

    g_pml4[511] = VMA2PA(g_pdpt_kern) | RW;
    g_pdpt_kern[510] = VMA2PA(g_pd_kern) | RW;
    for (int i = 0; i < 512; ++i) {
        g_pd_kern[i] = (OB_KERNEL_LMA + ((uint64_t)i << 21)) | RWH;
    }

    uint64_t cr3 = VMA2PA(g_pml4);
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");

    printk("[VMM] 4-level paging enabled\n");
    printk("[VMM] kernel high-half mapped RWH, DirectMap NX\n");

    g_kernel_pml4 = vmm_current_pml4();
}

uint64_t *vmm_current_pml4(void)
{
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    uint64_t pa = cr3 & PTE_ADDR_MASK;
    return (uint64_t *)(DIRECTMAP_BASE + pa);
}

int vmm_map_2mb(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t i4 = PML4_INDEX(virt);
    uint64_t i3 = PDPT_INDEX(virt);
    uint64_t i2 = PD_INDEX(virt);

    if (!(pml4[i4] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE +
                                  (pml4[i4] & PTE_ADDR_MASK));
    if (!(pdpt[i3] & PTE_PRESENT)) return -1;
    uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE +
                                (pdpt[i3] & PTE_ADDR_MASK));
    if (pd[i2] & PTE_PRESENT) return -2;

    pd[i2] = (phys & ~0x1FFFFFULL) | flags;
    return 0;
}

int vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t i4 = PML4_INDEX(virt);
    uint64_t i3 = PDPT_INDEX(virt);
    uint64_t i2 = PD_INDEX(virt);
    uint64_t i1 = PT_INDEX(virt);

    const uint64_t user_flag = (flags & PTE_USER) ? PTE_USER : 0;

    if (!(pml4[i4] & PTE_PRESENT)) {
        struct page *pg = vmm_alloc_pt_page();
        if (!pg) return -1;
        pml4[i4] = page_to_phys(pg) | PTE_PRESENT | PTE_WRITABLE | user_flag;
    }
    uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE + (pml4[i4] & PTE_ADDR_MASK));

    if (!(pdpt[i3] & PTE_PRESENT)) {
        struct page *pg = vmm_alloc_pt_page();
        if (!pg) return -1;
        pdpt[i3] = page_to_phys(pg) | PTE_PRESENT | PTE_WRITABLE | user_flag;
    }
    uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE + (pdpt[i3] & PTE_ADDR_MASK));

    if (pd[i2] & PTE_HUGE) {
        serial_printf("[VMM] map_page: va=0x%llx hits HUGE at pd[%llu]=0x%llx\n",
                      (unsigned long long)virt, (unsigned long long)i2,
                      (unsigned long long)pd[i2]);
        return -2;
    }
    if (!(pd[i2] & PTE_PRESENT)) {
        struct page *pg = vmm_alloc_pt_page();
        if (!pg) return -1;
        pd[i2] = page_to_phys(pg) | PTE_PRESENT | PTE_WRITABLE | user_flag;
    }
    uint64_t *pt = (uint64_t *)(DIRECTMAP_BASE + (pd[i2] & PTE_ADDR_MASK));

    if (pt[i1] & PTE_PRESENT) {
        serial_printf("[VMM] map_page: va=0x%llx pt[%llu] already present "
                      "= 0x%llx (existing va?)\n",
                      (unsigned long long)virt, (unsigned long long)i1,
                      (unsigned long long)pt[i1]);
        return -2;
    }
    pt[i1] = (phys & PTE_ADDR_MASK) | flags;
    return 0;
}

int vmm_unmap_page(uint64_t *pml4, uint64_t virt)
{
    uint64_t *pte = vmm_get_pte(pml4, virt);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    *pte = 0;
    vmm_flush_tlb();
    return 0;
}

uint64_t *vmm_get_pte(uint64_t *pml4, uint64_t virt)
{
    uint64_t i4 = PML4_INDEX(virt);
    uint64_t i3 = PDPT_INDEX(virt);
    uint64_t i2 = PD_INDEX(virt);
    uint64_t i1 = PT_INDEX(virt);

    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE +
                                  (pml4[i4] & PTE_ADDR_MASK));
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE +
                                (pdpt[i3] & PTE_ADDR_MASK));
    if (!(pd[i2] & PTE_PRESENT)) return 0;
    if (pd[i2] & PTE_HUGE) return 0;
    uint64_t *pt = (uint64_t *)(DIRECTMAP_BASE + (pd[i2] & PTE_ADDR_MASK));
    return &pt[i1];
}

uint64_t *vmm_create_address_space(void)
{
    struct page *pg = vmm_alloc_pt_page();
    if (!pg) return 0;

    uint64_t pa = page_to_phys(pg);
    uint64_t *new_pml4 = (uint64_t *)(DIRECTMAP_BASE + pa);

    for (int i = 0; i < 512; ++i) new_pml4[i] = 0;

    uint64_t *cur = vmm_current_pml4();
    new_pml4[0]   = cur[0];
    new_pml4[256] = cur[256];
    new_pml4[511] = cur[511];

    return new_pml4;
}

void vmm_destroy_address_space(uint64_t *pml4)
{
    if (!pml4) return;
    if (pml4 == g_kernel_pml4) {
        printk("[VMM] WARN: refuse to destroy kernel PML4\n");
        return;
    }
    uint64_t pa = (uint64_t)pml4 - DIRECTMAP_BASE;
    if (!vmm_is_pt_page_strict(pa)) {
        printk("[VMM] WARN: destroy PML4 pa=0x%llx not marked, skip\n",
               (unsigned long long)pa);
        return;
    }
    vmm_free_pt_page_ex(pa, 0xFFFFFFFFu);
}

void vmm_switch_address_space(uint64_t *pml4)
{
    if (!pml4) return;
    uint64_t pa = (uint64_t)pml4 - DIRECTMAP_BASE;
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(pa) : "memory");
}

void vmm_flush_tlb(void)
{
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

void vmm_copy_kernel_mappings(uint64_t *dst_pml4, uint64_t *src_pml4)
{
    if (!dst_pml4 || !src_pml4) return;
    dst_pml4[0]   = src_pml4[0];
    dst_pml4[256] = src_pml4[256];
    dst_pml4[511] = src_pml4[511];
}

void vmm_enable_smep_smap(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);
    int has_smep = (ebx >> 7) & 1;
    int has_smap = (ebx >> 20) & 1;

    uint64_t cr4 = read_cr4();
    if (has_smep) { cr4 |= (1ULL << 20); g_smep_enabled = 1; }
    else printk("[VMM] SMEP not supported by CPU\n");
    if (has_smap) { cr4 |= (1ULL << 21); g_smap_enabled = 1; }
    else printk("[VMM] SMAP not supported by CPU\n");
    write_cr4(cr4);

    printk("[VMM] SMEP=%s SMAP=%s (CR4=0x%llx)\n",
           g_smep_enabled ? "on" : "off",
           g_smap_enabled ? "on" : "off",
           (unsigned long long)read_cr4());
}

int vmm_smep_enabled(void) { return g_smep_enabled; }
int vmm_smap_enabled(void) { return g_smap_enabled; }

uint64_t *vmm_kernel_pml4(void) { return g_kernel_pml4; }

int vmm_setup_user_space(uint64_t *pml4, uint64_t base, uint64_t limit)
{
    if (!pml4) return -1;
    if (base >= limit) return -1;
    if (limit > USER_SPACE_END + 1ULL) return -1;

    for (uint64_t v = base; v < limit; ) {
        uint64_t i4 = PML4_INDEX(v);
        if (i4 >= 256) return -1;

        if (pml4[i4] & PTE_PRESENT) {
            uint64_t existing_pa = pml4[i4] & PTE_ADDR_MASK;
            if (!vmm_is_pt_page_strict(existing_pa)) {
                pml4[i4] = 0;
            }
        }
        if (!(pml4[i4] & PTE_PRESENT)) {
            struct page *pg = vmm_alloc_pt_page();
            if (!pg) return -1;
            pml4[i4] = page_to_phys(pg)
                     | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE +
                                      (pml4[i4] & PTE_ADDR_MASK));
        uint64_t i3 = PDPT_INDEX(v);

        if (pdpt[i3] & PTE_PRESENT) {
            uint64_t existing_pa = pdpt[i3] & PTE_ADDR_MASK;
            if (!vmm_is_pt_page_strict(existing_pa)) {
                pdpt[i3] = 0;
            }
        }
        if (!(pdpt[i3] & PTE_PRESENT)) {
            struct page *pg = vmm_alloc_pt_page();
            if (!pg) return -1;
            pdpt[i3] = page_to_phys(pg)
                     | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }
        uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE +
                                    (pdpt[i3] & PTE_ADDR_MASK));
        uint64_t i2 = PD_INDEX(v);

        if (pd[i2] & PTE_HUGE) return -1;
        if (pd[i2] & PTE_PRESENT) {
            uint64_t existing_pa = pd[i2] & PTE_ADDR_MASK;
            if (!vmm_is_pt_page_strict(existing_pa)) {
                pd[i2] = 0;
            }
        }
        if (!(pd[i2] & PTE_PRESENT)) {
            struct page *pg = vmm_alloc_pt_page();
            if (!pg) return -1;
            pd[i2] = page_to_phys(pg)
                   | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }

        uint64_t next = (v & ~((1ULL << 21) - 1)) + (1ULL << 21);
        if (next > limit) next = limit;
        v = next;
    }
    return 0;
}

void vmm_free_user_pagetables(uint64_t *pml4)
{
    if (!pml4) return;
    if (pml4 == g_kernel_pml4) {
        printk("[VMM] WARN: refuse to free kernel PML4\n");
        return;
    }

    for (int i4 = 0; i4 < 256; ++i4) {
        uint64_t pml4e = pml4[i4];
        if (!(pml4e & PTE_PRESENT)) continue;
        if (!(pml4e & PTE_USER)) { pml4[i4] = 0; continue; }

        uint64_t pdpt_pa = pml4e & PTE_ADDR_MASK;
        pml4[i4] = 0;

        if (!vmm_is_pt_page_strict(pdpt_pa)) {
            printk("[VMM] WARN: pml4[%d] pa=0x%llx not a PT page, skip\n",
                   i4, (unsigned long long)pdpt_pa);
            continue;
        }

        uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE + pdpt_pa);
        for (int i3 = 0; i3 < 512; ++i3) {
            uint64_t pdpte = pdpt[i3];
            if (!(pdpte & PTE_PRESENT)) continue;
            if (pdpte & PTE_HUGE) { pdpt[i3] = 0; continue; }

            uint64_t pd_pa = pdpte & PTE_ADDR_MASK;
            pdpt[i3] = 0;

            if (!(pdpte & PTE_USER)) continue;
            if (!vmm_is_pt_page_strict(pd_pa)) {
                printk("[VMM] WARN: pdpt[%d] pa=0x%llx not a PT page\n",
                       i3, (unsigned long long)pd_pa);
                continue;
            }

            uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE + pd_pa);
            for (int i2 = 0; i2 < 512; ++i2) {
                uint64_t pde = pd[i2];
                if (!(pde & PTE_PRESENT)) continue;
                if (pde & PTE_HUGE) { pd[i2] = 0; continue; }

                uint64_t pt_pa = pde & PTE_ADDR_MASK;
                pd[i2] = 0;

                if (!(pde & PTE_USER)) continue;
                if (!vmm_is_pt_page_strict(pt_pa)) {
                    printk("[VMM] WARN: pd[%d] pa=0x%llx not a PT page\n",
                           i2, (unsigned long long)pt_pa);
                    continue;
                }
                vmm_free_pt_page_ex(pt_pa, 0);
            }
            vmm_free_pt_page_ex(pd_pa, 0);
        }
        vmm_free_pt_page_ex(pdpt_pa, 0);
    }
}

int vmm_is_pt_page(uint64_t pa)
{
    if (pa & 0xFFFULL) return 0;
    if (pa < PMM_PHYS_LOW_LIMIT) return 0;
    if (pa >= PMM_PHYS_HIGH_LIMIT) return 0;
    return pt_bit_test(pa) ? 1 : 0;
}

/* ★★★ 本轮新增：对外暴露严格判定 ★★★ */
int vmm_is_pt_page_strict_pub(uint64_t pa)
{
    return vmm_is_pt_page_strict(pa);
}
/*===OmniBridgeOs/kernel/arch/x64/vmm.c 结束===*/