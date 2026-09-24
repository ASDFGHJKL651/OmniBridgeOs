/*===OmniBridgeOs/kernel/arch/x64/shm.c===*/
#include "shm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "vmm.h"
#include "serial.h"
#include "task.h"
#include "user/user.h"

#define SHM_MAX_REGIONS 64
static struct shm_region *g_shm_table[SHM_MAX_REGIONS];
static uint32_t g_shm_next_id = 1;
static spinlock_t g_shm_table_lock = SPINLOCK_INIT;

static int size_to_order_(uint64_t size)
{
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while (order < MAX_ORDER && ((uint64_t)1 << order) < pages) order++;
    return order;
}

struct shm_region *shm_create(uint64_t size)
{
    if (size == 0) return 0;

    int order = size_to_order_(size);
    if (order >= MAX_ORDER) return 0;

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) return 0;

    uint64_t pa   = page_to_phys(pg);
    void    *addr = (void *)(uintptr_t)(DIRECTMAP_BASE + pa);
    uint64_t actual = PAGE_SIZE << order;

    uint8_t *p = (uint8_t *)addr;
    for (uint64_t i = 0; i < actual; ++i) p[i] = 0;

    struct shm_region *r = (struct shm_region *)kzalloc(sizeof(*r));
    if (!r) { pmm_free_pages(pg, order); return 0; }

    r->addr     = addr;
    r->size     = actual;
    r->order    = order;
    r->refcount = 1;
    spin_lock_init(&r->lock);

    uint64_t fl;
    spin_lock_irqsave(&g_shm_table_lock, &fl);
    for (int i = 0; i < SHM_MAX_REGIONS; ++i) {
        if (!g_shm_table[i]) {
            r->shm_id = g_shm_next_id++;
            g_shm_table[i] = r;
            break;
        }
    }
    spin_unlock_irqrestore(&g_shm_table_lock, fl);

    serial_printf("[SHM] create id=%u addr=0x%llx size=%llu\n",
                  (unsigned)r->shm_id,
                  (unsigned long long)(uintptr_t)addr,
                  (unsigned long long)actual);
    return r;
}

void shm_retain(struct shm_region *r)
{
    if (!r) return;
    uint64_t fl;
    spin_lock_irqsave(&r->lock, &fl);
    r->refcount++;
    spin_unlock_irqrestore(&r->lock, fl);
}

void shm_release(struct shm_region *r)
{
    if (!r) return;

    uint64_t fl;
    spin_lock_irqsave(&r->lock, &fl);
    if (r->refcount == 0) {
        spin_unlock_irqrestore(&r->lock, fl);
        return;
    }
    r->refcount--;
    uint32_t rc = r->refcount;
    spin_unlock_irqrestore(&r->lock, fl);
    if (rc > 0) return;

    uint32_t id = r->shm_id;
    uint64_t fl2;
    spin_lock_irqsave(&g_shm_table_lock, &fl2);
    for (int i = 0; i < SHM_MAX_REGIONS; ++i) {
        if (g_shm_table[i] == r) { g_shm_table[i] = 0; break; }
    }
    spin_unlock_irqrestore(&g_shm_table_lock, fl2);

    uint64_t pa    = (uint64_t)(uintptr_t)r->addr - DIRECTMAP_BASE;
    struct page *pg = phys_to_page(pa);
    int order = r->order;

    serial_printf("[SHM] release id=%u\n", (unsigned)id);
    kfree(r);
    pmm_free_pages(pg, order);
}

void *shm_addr(struct shm_region *r) { return r ? r->addr : 0; }

uint32_t shm_refcount(struct shm_region *r)
{
    if (!r) return 0;
    uint64_t fl;
    spin_lock_irqsave(&r->lock, &fl);
    uint32_t n = r->refcount;
    spin_unlock_irqrestore(&r->lock, fl);
    return n;
}

struct shm_region *shm_find_by_id(uint32_t id)
{
    struct shm_region *r = 0;
    uint64_t fl;
    spin_lock_irqsave(&g_shm_table_lock, &fl);
    for (int i = 0; i < SHM_MAX_REGIONS; ++i) {
        if (g_shm_table[i] && g_shm_table[i]->shm_id == id) {
            r = g_shm_table[i]; break;
        }
    }
    spin_unlock_irqrestore(&g_shm_table_lock, fl);
    return r;
}

int shm_map_into_task(struct task_t *t, struct shm_region *r, uint64_t uaddr)
{
    if (!t || !r) return -22;

    struct user_ctx *uc = user_get_ctx(t);
    uint64_t *pml4 = 0;
    if (t->priv_iso_ready && t->mem_domain.pml4_self_ptr) {
        pml4 = t->mem_domain.pml4_self_ptr;
    } else if (uc && uc->pml4) {
        pml4 = uc->pml4;
    } else {
        return -1;
    }

    uint64_t npages = r->size / PAGE_SIZE;
    uint64_t phys   = (uint64_t)(uintptr_t)r->addr - DIRECTMAP_BASE;

    for (uint64_t i = 0; i < npages; ++i) {
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
        int rc = vmm_map_page(pml4, uaddr + i * PAGE_SIZE,
                              phys + i * PAGE_SIZE, flags);
        if (rc != 0) {
            for (uint64_t j = 0; j < i; ++j) {
                vmm_unmap_page(pml4, uaddr + j * PAGE_SIZE);
            }
            return -5;
        }
    }
    vmm_flush_tlb();
    return 0;
}

int64_t shm_sys_create(struct task_t *cur, uint64_t size)
{
    if (!cur) return -1;
    if (size == 0 || size > 64 * 1024 * 1024) return -22;

    struct shm_region *r = shm_create(size);
    if (!r) return -12;
    return (int64_t)r->shm_id;
}

int64_t shm_sys_map(struct task_t *cur, uint64_t shm_id, uint64_t uaddr)
{
    if (!cur) return -1;

    struct shm_region *r = shm_find_by_id((uint32_t)shm_id);
    if (!r) return -2;

    if (uaddr == 0) {
        struct user_ctx *uc = user_get_ctx(cur);
        if (!uc) return -1;
        uaddr = uc->heap_cur;
        uc->heap_cur += r->size;
        if (uc->heap_cur > uc->heap_end) return -12;
    }

    if (!user_range_ok(uaddr, r->size)) return -14;

    int rc = shm_map_into_task(cur, r, uaddr);
    if (rc != 0) return rc;

    struct shm_mapping *m = (struct shm_mapping *)kzalloc(sizeof(*m));
    if (!m) return -12;
    m->region = r;
    m->uaddr  = uaddr;
    m->size   = r->size;
    m->phys_base = (uint64_t)(uintptr_t)r->addr - DIRECTMAP_BASE;
    m->next   = cur->shm_list;
    cur->shm_list = m;

    shm_retain(r);
    serial_printf("[SHM] mapped id=%u uaddr=0x%llx\n",
                  (unsigned)shm_id, (unsigned long long)uaddr);
    return (int64_t)uaddr;
}

/*
 * ★★★ 修复 4：shm_sys_unmap 真正 unmap 用户页表 ★★★
 *
 * 人工必须审查：
 *   - 旧实现仅从 shm_list 摘除映射记录，页表项仍指向已释放的物理页
 *     （UAF）。
 *   - 新实现顺序必须是：
 *       1) 先从用户页表 unmap（vmm_unmap_page 仅清 PTE，不释放物理页）；
 *       2) 再 shm_release（引用计数归零时释放物理页）。
 *   - 反序会导致 UAF。
 *   - 若同一 region 被映射两次到不同 uaddr，本函数只处理首次匹配的 uaddr；
 *     其他 uaddr 需单独 unmap。
 */
int shm_sys_unmap(struct task_t *cur, uint64_t uaddr, uint64_t size)
{
    (void)size;
    if (!cur) return -1;

    struct shm_mapping **pp = &cur->shm_list;
    while (*pp) {
        struct shm_mapping *m = *pp;
        if (m->uaddr == uaddr) {
            /* ★ 修复 4：先从用户页表 unmap，再 release */
            uint64_t *pml4 = 0;
            struct user_ctx *uc = user_get_ctx(cur);
            if (cur->priv_iso_ready && cur->mem_domain.pml4_self_ptr) {
                pml4 = cur->mem_domain.pml4_self_ptr;
            } else if (uc && uc->pml4) {
                pml4 = uc->pml4;
            }

            if (pml4) {
                uint64_t npages = m->size / PAGE_SIZE;
                for (uint64_t i = 0; i < npages; ++i) {
                    vmm_unmap_page(pml4, m->uaddr + i * PAGE_SIZE);
                }
                vmm_flush_tlb();
            }

            *pp = m->next;
            shm_release(m->region);
            kfree(m);
            serial_printf("[SHM] unmapped uaddr=0x%llx\n",
                          (unsigned long long)uaddr);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -2;
}

void shm_release_mappings(struct task_t *t)
{
    if (!t) return;
    struct shm_mapping *m = t->shm_list;
    t->shm_list = 0;
    while (m) {
        struct shm_mapping *n = m->next;
        shm_release(m->region);
        kfree(m);
        m = n;
    }
}
/*===OmniBridgeOs/kernel/arch/x64/shm.c 结束===*/