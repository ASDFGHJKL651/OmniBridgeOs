/*===OmniBridgeOs/kernel/arch/x64/mem_domain.c===*/
#include "mem_domain.h"
#include "vmm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "serial.h"
#include "vfs.h"

static uint64_t domain_max_pages(uint8_t priv)
{
    if (priv == 0) return MEM_DOMAIN_PRIV0_MAX_PAGES;
    if (priv == 1) return MEM_DOMAIN_PRIV1_MAX_PAGES;
    return 0;
}

static uint64_t domain_init_pages(uint8_t priv)
{
    if (priv == 0) return MEM_DOMAIN_PRIV0_INIT_PAGES;
    if (priv == 1) return MEM_DOMAIN_PRIV1_INIT_PAGES;
    return 0;
}

int mem_domain_contains(const struct task_t *t, uint64_t vaddr)
{
    if (!t) return 0;
    if (t->mem_domain.limit_vaddr == 0) return 0;
    return vaddr >= t->mem_domain.base_vaddr &&
           vaddr <  t->mem_domain.limit_vaddr;
}

uint64_t mem_domain_used_pages(const struct task_t *t)
{
    return t ? (uint64_t)t->mem_domain_page_count : 0ULL;
}

int mem_domain_grow(struct task_t *t, uint64_t n)
{
    if (!t || !t->mem_domain.pml4_self_ptr || !t->mem_domain_pages) {
        return OB_EINVAL;
    }
    if (n == 0) return 0;

    uint64_t maxp = domain_max_pages(t->privilege_level);
    uint64_t cur  = (uint64_t)t->mem_domain_page_count;
    if (cur + n > maxp) return OB_ENOMEM;
    if (cur + n > (uint64_t)MEM_DOMAIN_MAX_PAGES) return OB_ENOMEM;

    struct page **arr = (struct page **)t->mem_domain_pages;
    uint32_t start = t->mem_domain_page_count;

    serial_printf("[MEM-DOMAIN-DBG] grow: t=%p n=%llu start=%u\n",
                  (void *)t,
                  (unsigned long long)n,
                  (unsigned)start);

    uint64_t i;
    for (i = 0; i < n; ++i) {
        struct page *pg = pmm_alloc_pages(0);
        if (!pg) {
            serial_printf("[MEM-DOMAIN-DBG] grow: pmm_alloc_pages failed i=%llu\n",
                          (unsigned long long)i);
            break;
        }

        uint64_t pa = page_to_phys(pg);
        uint64_t va = t->mem_domain.base_vaddr +
                      (uint64_t)(start + (uint32_t)i) * PAGE_SIZE;

        serial_printf("[MEM-DOMAIN-DBG] grow: i=%llu pa=0x%llx va=0x%llx\n",
                      (unsigned long long)i,
                      (unsigned long long)pa,
                      (unsigned long long)va);

        int rc = vmm_map_page(t->mem_domain.pml4_self_ptr, va, pa,
                              PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        if (rc != 0) {
            serial_printf("[MEM-DOMAIN-DBG] grow: vmm_map_page rc=%d\n", rc);
            pmm_free_pages(pg, 0);
            break;
        }
        arr[start + i] = pg;
    }

    if (i < n) {
        for (uint64_t j = 0; j < i; ++j) {
            uint64_t va = t->mem_domain.base_vaddr +
                          (uint64_t)(start + (uint32_t)j) * PAGE_SIZE;
            vmm_unmap_page(t->mem_domain.pml4_self_ptr, va);
            if (arr[start + j]) {
                pmm_free_pages(arr[start + j], 0);
                arr[start + j] = 0;
            }
        }
        return OB_ENOMEM;
    }

    t->mem_domain_page_count       += (uint32_t)n;
    t->mem_domain.phys_frame_count  = t->mem_domain_page_count;
    serial_printf("[MEM-DOMAIN-DBG] grow: done i=%llu\n",
                  (unsigned long long)i);
    return 0;
}

int mem_domain_create(struct task_t *t)
{
    serial_printf("[MEM-DOMAIN-DBG] create: t=%p pid=%llu priv=%u\n",
                  (void *)t,
                  (unsigned long long)(t ? t->pid : 0),
                  (unsigned)(t ? t->privilege_level : 0xFF));

    if (!t) return OB_EINVAL;
    if (t->privilege_level > 1) return OB_EINVAL;
    if (t->priv_iso_ready) return OB_EAGAIN;
    if (t->mem_domain.pml4_self_ptr) return OB_EAGAIN;

    if (t->privilege_level == 0) {
        t->isolation_mode = ISOLATION_MEM;
        t->mem_domain.base_vaddr  = MEM_DOMAIN_PRIV0_BASE_VADDR;
        t->mem_domain.limit_vaddr = MEM_DOMAIN_PRIV0_LIMIT_VADDR;
    } else {
        t->isolation_mode = ISOLATION_TMPDIR;
        t->mem_domain.base_vaddr  = MEM_DOMAIN_PRIV1_BASE_VADDR;
        t->mem_domain.limit_vaddr = MEM_DOMAIN_PRIV1_LIMIT_VADDR;
    }

    serial_printf("[MEM-DOMAIN-DBG] create: before vmm_create_address_space\n");
    uint64_t *pml4 = vmm_create_address_space();
    if (!pml4) return OB_ENOMEM;
    serial_printf("[MEM-DOMAIN-DBG] create: after vmm_create_address_space "
                  "pml4=%p\n", (void *)pml4);

    serial_printf("[MEM-DOMAIN-DBG] create: before vmm_setup_user_space\n");
    int rc = vmm_setup_user_space(pml4,
                                  t->mem_domain.base_vaddr,
                                  t->mem_domain.limit_vaddr);
    if (rc != 0) {
        vmm_free_user_pagetables(pml4);
        vmm_destroy_address_space(pml4);
        return OB_ENOMEM;
    }
    serial_printf("[MEM-DOMAIN-DBG] create: after vmm_setup_user_space\n");

    serial_printf("[MEM-DOMAIN-DBG] create: before kzalloc(%llu)\n",
                  (unsigned long long)(MEM_DOMAIN_MAX_PAGES * sizeof(void *)));
    void *arr = kzalloc((size_t)MEM_DOMAIN_MAX_PAGES * sizeof(void *));
    if (!arr) {
        vmm_free_user_pagetables(pml4);
        vmm_destroy_address_space(pml4);
        return OB_ENOMEM;
    }
    serial_printf("[MEM-DOMAIN-DBG] create: after kzalloc arr=%p\n", arr);

    t->mem_domain_pages         = arr;
    t->mem_domain_page_count    = 0;
    t->mem_domain.pml4_self_ptr = pml4;
    t->mem_domain.phys_frame_count = 0;
    t->mem_domain.sandbox_owned = 0;

    uint64_t n = domain_init_pages(t->privilege_level);
    if (n > 0) {
        serial_printf("[MEM-DOMAIN-DBG] create: before mem_domain_grow n=%llu\n",
                      (unsigned long long)n);
        rc = mem_domain_grow(t, n);
        serial_printf("[MEM-DOMAIN-DBG] create: after mem_domain_grow rc=%d\n",
                      rc);
        if (rc != 0) {
            kfree(arr);
            t->mem_domain_pages = 0;
            vmm_free_user_pagetables(pml4);
            vmm_destroy_address_space(pml4);
            t->mem_domain.pml4_self_ptr = 0;
            return rc;
        }
    }

    serial_printf("[MEM-DOMAIN-DBG] create: final serial_printf\n");
    serial_printf("[MEM-DOMAIN] create pid=%llu priv=%u "
                  "base=0x%llx limit=0x%llx pages=%llu pml4=0x%llx\n",
                  (unsigned long long)t->pid,
                  (unsigned)t->privilege_level,
                  (unsigned long long)t->mem_domain.base_vaddr,
                  (unsigned long long)t->mem_domain.limit_vaddr,
                  (unsigned long long)t->mem_domain_page_count,
                  (unsigned long long)(uintptr_t)pml4);
    serial_printf("[MEM-DOMAIN-DBG] create: returning 0\n");
    return 0;
}

void mem_domain_destroy(struct task_t *t)
{
    if (!t) return;

    serial_printf("[MEM-DOMAIN-DBG] destroy: t=%p pid=%llu\n",
                  (void *)t, (unsigned long long)t->pid);

    if (!t->mem_domain.pml4_self_ptr) {
        if (t->mem_domain_pages) {
            kfree(t->mem_domain_pages);
            t->mem_domain_pages = 0;
        }
        t->mem_domain_page_count = 0;
        t->mem_domain.phys_frame_count = 0;
        t->isolation_mode = ISOLATION_NONE;
        return;
    }

    /* ★ 若该 PML4 就是当前 CR3 指向的，先切回内核 PML4 */
    uint64_t *cur = vmm_current_pml4();
    if (cur == t->mem_domain.pml4_self_ptr) {
        uint64_t *kpml4 = vmm_kernel_pml4();
        if (kpml4) vmm_switch_address_space(kpml4);
    }

    struct page **arr = (struct page **)t->mem_domain_pages;
    if (arr) {
        for (uint32_t i = 0; i < t->mem_domain_page_count; ++i) {
            if (!arr[i]) continue;
            uint64_t va = t->mem_domain.base_vaddr +
                          (uint64_t)i * PAGE_SIZE;
            vmm_unmap_page(t->mem_domain.pml4_self_ptr, va);
            pmm_free_pages(arr[i], 0);
            arr[i] = 0;
        }
    }

    vmm_flush_tlb();

    vmm_free_user_pagetables(t->mem_domain.pml4_self_ptr);
    vmm_destroy_address_space(t->mem_domain.pml4_self_ptr);

    t->mem_domain.pml4_self_ptr    = 0;
    t->mem_domain.phys_frame_count = 0;
    t->mem_domain.base_vaddr       = 0;
    t->mem_domain.limit_vaddr      = 0;
    t->isolation_mode              = ISOLATION_NONE;

    if (t->mem_domain_pages) {
        kfree(t->mem_domain_pages);
        t->mem_domain_pages = 0;
    }
    t->mem_domain_page_count = 0;
}

int mem_domain_map_page(struct task_t *t, uint64_t vaddr,
                        uint64_t phys, uint64_t flags)
{
    if (!t || !t->mem_domain.pml4_self_ptr) return OB_EINVAL;
    if (!mem_domain_contains(t, vaddr)) return OB_EINVAL;
    return vmm_map_page(t->mem_domain.pml4_self_ptr, vaddr, phys,
                        flags | PTE_USER);
}
/*===OmniBridgeOs/kernel/arch/x64/mem_domain.c 结束===*/