/*===OmniBridgeOs/kernel/arch/x64/pmm.c===*/
#include "pmm.h"
#include "printk.h"
#include "serial.h"
#include "spinlock.h"

/*
 * 物理内存伙伴系统 —— SMP 安全版本。
 *
 * ★★★ 第 18C 步关键崩溃修复（第四版）★★★
 *
 *   症状：
 *     - OShell-TEST 的 login / passwd 三个用例失败
 *     - signal_test 触发内核态 #PF 并 panic
 *
 *   根因 A（BSS 未排除）：
 *     lld-link 生成的 .bss 段带 DISCARDABLE 标志，pe_to_elf.py 将其
 *     跳过 → elf_load 的 phys_end 仅到 .data 段结束 → pmm_init 未排
 *     除内核 BSS 所在的物理页 → 用户程序 brk 分配时覆写 g_users[]
 *     等 BSS 变量。
 *
 *     修复方式：在 pmm_init 中，无论 phys_end 计算是否准确，都把排除
 *     范围的下限扩展到 16MB（覆盖整个内核镜像 + BSS + 早期堆）。
 *
 *   根因 B（user_resume clobber）：
 *     见 user/user_resume.S 的注释。与本文件无关。
 *
 *   本版本改动：
 *     - 增加 MIN_KERNEL_END 兜底，确保排除范围至少到 16MB。
 */

struct page mem_map[MAX_PAGES];

static struct page *free_lists[MAX_ORDER];
static uint64_t total_pages = 0;
static uint64_t free_count  = 0;

static uint64_t g_total_pages_snapshot = 0;
static volatile int g_pmm_corruption_logged = 0;
static volatile uint64_t g_reserved_free_rejected = 0;

static spinlock_t pmm_lock = SPINLOCK_INIT;

static inline uint64_t pfn_of(const struct page *p)
{
    return (uint64_t)(p - mem_map);
}

static inline struct page *buddy_of(const struct page *p, int order)
{
    uint64_t pfn = pfn_of(p) ^ (1ULL << order);
    return &mem_map[pfn];
}

static void free_list_push(int order, struct page *p)
{
    p->order = (uint8_t)order;
    p->flags = PG_FREE;
    p->next  = free_lists[order];
    free_lists[order] = p;
}

static struct page *free_list_pop(int order)
{
    while (free_lists[order]) {
        struct page *p = free_lists[order];

        if (!(p->flags & PG_FREE)) {
            printk(KERN_ERR
                   "[PMM] free_list[%d] head pfn=%llu flags=0x%x "
                   "not PG_FREE, discarding from list\n",
                   order,
                   (unsigned long long)pfn_of(p),
                   (unsigned)p->flags);
            free_lists[order] = p->next;
            p->next  = 0;
            continue;
        }

        free_lists[order] = p->next;
        p->next  = 0;
        p->flags = 0;
        return p;
    }
    return 0;
}

static void mark_region_usable(uint64_t start_phys, uint64_t pages)
{
    uint64_t start_pfn = start_phys >> PAGE_SHIFT;
    uint64_t end_pfn   = start_pfn + pages;
    if (end_pfn > MAX_PAGES) end_pfn = MAX_PAGES;
    if (start_pfn >= end_pfn) return;

    uint64_t pfn = start_pfn;
    while (pfn < end_pfn) {
        int order = MAX_ORDER - 1;
        while (order > 0) {
            uint64_t block_pages = 1ULL << order;
            if ((pfn & (block_pages - 1)) == 0 && pfn + block_pages <= end_pfn)
                break;
            --order;
        }
        uint64_t block_pages = 1ULL << order;
        struct page *p = &mem_map[pfn];

        for (uint64_t i = 0; i < block_pages; ++i) {
            mem_map[pfn + i].order      = 0;
            mem_map[pfn + i].flags      = 0;
            mem_map[pfn + i].refcount   = 0;
            mem_map[pfn + i].slab_cache = 0;
            mem_map[pfn + i].slab_free  = 0;
            mem_map[pfn + i].next       = 0;
        }
        free_list_push(order, p);
        free_count  += block_pages;
        total_pages += block_pages;
        pfn += block_pages;
    }
}

void pmm_init(const struct ob_boot_info *bi)
{
    for (uint64_t i = 0; i < MAX_PAGES; ++i) {
        mem_map[i].order      = 0;
        mem_map[i].flags      = PG_RESERVED;
        mem_map[i].refcount   = 1;
        mem_map[i].slab_cache = 0;
        mem_map[i].slab_free  = 0;
        mem_map[i].next       = 0;
    }
    for (int i = 0; i < MAX_ORDER; ++i) free_lists[i] = 0;

    spin_lock_init(&pmm_lock);

    total_pages = 0;
    free_count  = 0;
    g_total_pages_snapshot = 0;
    g_pmm_corruption_logged = 0;
    g_reserved_free_rejected = 0;

    if (!bi) {
        printk("[PMM] no boot_info, no pages registered\n");
        return;
    }

    /* ============================================================
     * ★ 第 18C 步：确定内核镜像物理范围
     * ============================================================ */
    uint64_t kern_start = bi->kernel_phys_start;
    uint64_t kern_end   = bi->kernel_phys_end;

    if (kern_start == 0 || kern_end <= kern_start) {
        kern_start = 0x200000ULL;
        kern_end   = 0x400000ULL;
        printk("[PMM] WARN: boot_info kernel range missing, "
               "using conservative default [0x%llx, 0x%llx)\n",
               (unsigned long long)kern_start,
               (unsigned long long)kern_end);
    } else {
        kern_start &= PAGE_MASK;
        kern_end    = (kern_end + PAGE_SIZE - 1) & PAGE_MASK;
    }

    /* ★★★ 第 18C 步关键兜底：确保排除范围至少到 16MB ★★★
     *
     * 症状（BSS 未排除）：
     *   lld-link 生成的 .bss 段带 DISCARDABLE 标志时，pe_to_elf.py
     *   会把它跳过，导致 phys_end 只到 .data 段末尾。此时 BSS 的
     *   mem_map[131072]（约 4MB）等变量未被排除，用户程序 brk 分配
     *   可能覆写这些页，导致 g_users[] 等内核数据被清零。
     *
     * 保守兜底：
     *   内核镜像 LMA = 0x200000；实际大小 < 1MB；BSS 中含 mem_map
     *   4MB + 其他；总计 < 8MB。将排除范围扩展到 16MB 覆盖所有可能，
     *   浪费约 8MB 物理内存，对 1GB QEMU 内存完全可接受。
     *
     * 人工必须审查：
     *   若未来内核大小增长或 mem_map 扩大（MAX_PAGES 增加），需要
     *   同步上调 MIN_KERNEL_END。
     */
    const uint64_t MIN_KERNEL_END = 0x1000000ULL;   /* 16 MB */
    if (kern_end < MIN_KERNEL_END) {
        printk("[PMM] kernel range extended: [0x%llx, 0x%llx) → "
               "[0x%llx, 0x%llx) (16MB minimum)\n",
               (unsigned long long)kern_start,
               (unsigned long long)kern_end,
               (unsigned long long)kern_start,
               (unsigned long long)MIN_KERNEL_END);
        kern_end = MIN_KERNEL_END;
    }

    printk("[PMM] kernel image physical range reserved: "
           "[0x%llx, 0x%llx) (%llu KB)\n",
           (unsigned long long)kern_start,
           (unsigned long long)kern_end,
           (unsigned long long)((kern_end - kern_start) / 1024));

    /* ============================================================
     * 遍历 UEFI 内存图，标记可用区域（扣除内核范围）
     * ============================================================ */
    for (uint32_t i = 0; i < bi->entry_count; ++i) {
        const struct ob_memory_entry *e = &bi->memory_map[i];
        uint64_t phys  = e->physical_start;
        uint64_t pages = e->number_of_pages;

        int usable = (e->type == 7) || (e->type == 3) || (e->type == 4);
        if (!usable) continue;

        if (phys < 0x100000) {
            uint64_t skip = 0x100000 - phys;
            uint64_t skip_pages = (skip + PAGE_SIZE - 1) / PAGE_SIZE;
            if (skip_pages >= pages) continue;
            phys  += skip_pages * PAGE_SIZE;
            pages -= skip_pages;
        }

        uint64_t region_start = phys;
        uint64_t region_end   = phys + pages * PAGE_SIZE;

        if (region_end <= kern_start || region_start >= kern_end) {
            mark_region_usable(region_start, pages);
        } else {
            if (region_start < kern_start) {
                uint64_t head_pages = (kern_start - region_start) / PAGE_SIZE;
                if (head_pages > 0) {
                    mark_region_usable(region_start, head_pages);
                }
            }
            if (region_end > kern_end) {
                uint64_t tail_pages = (region_end - kern_end) / PAGE_SIZE;
                if (tail_pages > 0) {
                    mark_region_usable(kern_end, tail_pages);
                }
            }
        }
    }

    g_total_pages_snapshot = total_pages;

    printk("[PMM] usable pages: %llu, free: %llu\n",
           (unsigned long long)total_pages,
           (unsigned long long)free_count);
}

struct page *pmm_alloc_pages(int order)
{
    if (order < 0 || order >= MAX_ORDER) return 0;

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    int o = order;
    while (o < MAX_ORDER && !free_lists[o]) ++o;
    if (o == MAX_ORDER) {
        spin_unlock_irqrestore(&pmm_lock, flags);

        /* ★ 修复 1：OOM 标记（实际回收由 sched_tick -> oom_tick_reap） */
        extern int oom_mark_victim(void);
        oom_mark_victim();

        /* 重试一次 */
        spin_lock_irqsave(&pmm_lock, &flags);
        o = order;
        while (o < MAX_ORDER && !free_lists[o]) ++o;
        if (o == MAX_ORDER) {
            spin_unlock_irqrestore(&pmm_lock, flags);
            return 0;
        }
    }

    struct page *p = free_list_pop(o);

    if (!p || p < mem_map || p >= mem_map + MAX_PAGES) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        printk(KERN_ERR "[PMM] alloc: corrupted free list, bailing\n");
        return 0;
    }

    while (o > order) {
        --o;
        struct page *buddy = buddy_of(p, o);
        free_list_push(o, buddy);
    }
    p->order      = (uint8_t)order;
    p->flags      = 0;
    p->refcount   = 1;
    p->slab_cache = 0;
    p->slab_free  = 0;

    free_count -= (1ULL << order);

    spin_unlock_irqrestore(&pmm_lock, flags);
    return p;
}

void pmm_free_pages(struct page *p, int order)
{
    if (!p || order < 0 || order >= MAX_ORDER) return;

    if (p < mem_map || p >= mem_map + MAX_PAGES) {
        printk(KERN_ERR
               "[PMM] free_pages: invalid page pointer "
               "(out of mem_map range), ignored\n");
        return;
    }

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    if (g_total_pages_snapshot == 0) {
        if (!g_pmm_corruption_logged) {
            printk(KERN_ERR
                   "[PMM] free_pages: snapshot==0, PMM accounting "
                   "never initialized or corrupted; refusing to free\n");
            g_pmm_corruption_logged = 1;
        }
        spin_unlock_irqrestore(&pmm_lock, flags);
        return;
    }
    if (total_pages != g_total_pages_snapshot) {
        if (!g_pmm_corruption_logged) {
            printk(KERN_ERR
                   "[PMM] total_pages=%llu != snapshot=%llu — "
                   "counter corrupted, restoring snapshot\n",
                   (unsigned long long)total_pages,
                   (unsigned long long)g_total_pages_snapshot);
            g_pmm_corruption_logged = 1;
        }
        total_pages = g_total_pages_snapshot;
        spin_unlock_irqrestore(&pmm_lock, flags);
        return;
    }

    if (p->flags & PG_RESERVED) {
        g_reserved_free_rejected++;
        if (g_reserved_free_rejected <= 8) {
            printk(KERN_ERR
                   "[PMM] free_pages: pfn=%llu is PG_RESERVED "
                   "(not owned by pmm), refusing (count=%llu)\n",
                   (unsigned long long)pfn_of(p),
                   (unsigned long long)g_reserved_free_rejected);
        } else if (g_reserved_free_rejected == 9) {
            printk(KERN_ERR
                   "[PMM] free_pages: too many PG_RESERVED rejections, "
                   "suppressing further messages\n");
        }
        spin_unlock_irqrestore(&pmm_lock, flags);
        return;
    }

    if (p->flags & PG_FREE) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        printk("[PMM] double free detected (pfn=%llu), ignoring\n",
               (unsigned long long)pfn_of(p));
        return;
    }

    if ((int)p->order != order) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        printk(KERN_ERR
               "[PMM] free_pages: order mismatch "
               "(caller=%d, page_header=%d, pfn=%llu), ignored\n",
               order, (int)p->order,
               (unsigned long long)pfn_of(p));
        return;
    }

    const int original_order = order;

    while (order < MAX_ORDER - 1) {
        struct page *buddy = buddy_of(p, order);
        if (!(buddy->flags & PG_FREE) || buddy->order != order) break;

        struct page **pp = &free_lists[order];
        while (*pp && *pp != buddy) pp = &(*pp)->next;
        if (*pp == buddy) {
            *pp = buddy->next;
            buddy->next  = 0;
        } else {
            break;
        }
        if (buddy < p) p = buddy;
        ++order;
    }
    free_list_push(order, p);
    free_count += (1ULL << original_order);

    if (free_count > total_pages) {
        printk(KERN_ERR
               "[PMM] free_count %llu > total_pages %llu "
               "(orig_order=%d, merged_order=%d) — counter corruption, "
               "rollback this free\n",
               (unsigned long long)free_count,
               (unsigned long long)total_pages,
               original_order, order);
        free_count -= (1ULL << original_order);
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
}

uint64_t pmm_total_pages(void)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t v = total_pages;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return v;
}

uint64_t pmm_free_pages_count(void)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    uint64_t v = free_count;
    spin_unlock_irqrestore(&pmm_lock, flags);
    return v;
}
/*===OmniBridgeOs/kernel/arch/x64/pmm.c 结束===*/