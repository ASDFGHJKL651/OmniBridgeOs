/*===OmniBridgeOs/kernel/arch/x64/user/user.c===*/
/*
 * user.c —— 用户态运行时的核心实现（第 18C 步）。
 *
 * 本文件关键修复摘要：
 *   1) 移除对 vmm_setup_user_space() 的显式预创建，改为完全依赖
 *      vmm_map_page() 的按需页表分配。
 *   2) setup_user_memory 在 TLS 页前 8 字节写入自引用（USER_TLS_BASE
 *      的值），使 pthread.c 的 tls_self() 能通过 %fs:0 拿到 tls_block*。
 *   3) 新增 user_thread_spawn：pthread 线程共享父 PML4，独立用户栈
 *      与 TLS 页。
 *   4) user_teardown 区分主线程（owns_pml4=1）与子线程（is_thread=1）。
 */
#include "user.h"
#include "signal.h"
#include "vmm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "serial.h"
#include "task.h"
#include "sched.h"
#include "vfs.h"
#include "../obr.h"
#include "elf_loader.h"

#define USER_IMAGE_BASE   0x0000008000000000ULL
#define USER_IMAGE_MAX    (256ULL * 1024 * 1024)

extern int vmm_is_pt_page_strict_pub(uint64_t pa);

extern struct page mem_map[];

static int g_user_inited = 0;
extern uint64_t g_syscall_kernel_rsp;

/*
 * ★ 修复 2：配额辅助函数
 *
 * 人工必须审查：
 *   - user_quota_alloc(t, n)  分配前调用，检查是否会超限；
 *   - user_quota_commit(t, n) 成功分配后调用，累加 mem_pages_used；
 *   - user_quota_release(t, n) 释放后调用，递减 mem_pages_used；
 *   - mem_quota_pages == 0 表示不限制。
 *   - 单位是"页"（4KB），不是字节。
 */
static int user_quota_alloc(struct task_t *t, uint32_t n)
{
    if (!t || t->mem_quota_pages == 0) return 0;
    uint64_t after = (uint64_t)t->mem_pages_used + n;
    if (after > t->mem_quota_pages) {
        serial_printf("[QUOTA] pid=%llu mem quota exceeded: "
                      "%u + %u > %u\n",
                      (unsigned long long)t->pid,
                      (unsigned)t->mem_pages_used,
                      (unsigned)n,
                      (unsigned)t->mem_quota_pages);
        return OB_ENOMEM;
    }
    return 0;
}

static void user_quota_commit(struct task_t *t, uint32_t n)
{
    if (!t) return;
    t->mem_pages_used += n;
}

static void user_quota_release(struct task_t *t, uint32_t n)
{
    if (!t) return;
    if (t->mem_pages_used >= n) t->mem_pages_used -= n;
    else                        t->mem_pages_used = 0;
}

void user_entry_marker(void *arg) { (void)arg; for (;;) __asm__ __volatile__("hlt"); }

struct user_ctx *user_get_ctx(struct task_t *t) {
    if (!t || !t->user_ctx) return 0;
    return (struct user_ctx *)t->user_ctx;
}

uint64_t *user_task_pml4(struct task_t *t)
{
    if (!t || !t->user_ctx) return 0;
    struct user_ctx *c = (struct user_ctx *)t->user_ctx;
    return c->pml4;
}

static struct user_ctx *ctx_alloc(struct task_t *t) {
    if (!t) return 0;
    if (t->user_ctx) return (struct user_ctx *)t->user_ctx;
    struct user_ctx *c = (struct user_ctx *)kzalloc(sizeof(*c));
    if (!c) return 0;
    uint8_t *p = (uint8_t *)c; for (unsigned i = 0; i < sizeof(*c); ++i) p[i] = 0;
    t->user_ctx = c; return c;
}
static void ctx_free(struct task_t *t) {
    if (!t || !t->user_ctx) return;
    kfree(t->user_ctx); t->user_ctx = 0;
}

void user_init(void) {
    if (g_user_inited) return;
    g_user_inited = 1;
    serial_printf("[USER] init: stack=[0x%llx,0x%llx) tls=0x%llx sigtramp=0x%llx image_base=0x%llx\n",
                  (unsigned long long)USER_STACK_BOTTOM, (unsigned long long)USER_STACK_TOP,
                  (unsigned long long)USER_TLS_BASE, (unsigned long long)USER_SIGTRAMP_ADDR,
                  (unsigned long long)USER_IMAGE_BASE);
}

int user_range_ok(uint64_t vaddr, uint64_t size) {
    if (size == 0) return 1;
    uint64_t end = vaddr + size;
    if (end < vaddr) return 0;
    if (vaddr < USER_SPACE_START) return 0;
    if (end - 1 > USER_SPACE_END) return 0;
    return 1;
}
void user_set_fsbase(uint64_t base) {
    uint32_t lo = (uint32_t)(base & 0xFFFFFFFF), hi = (uint32_t)(base >> 32);
    __asm__ __volatile__("wrmsr" :: "c"(0xC0000100u), "a"(lo), "d"(hi) : "memory");
}
uint64_t user_get_fsbase(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100u) : "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

int user_stack_write(struct user_ctx *c, uint64_t uva, const void *src, uint64_t len) {
    if (!c || !src) return -1;
    if (uva < c->stack_bottom || uva + len > c->stack_top) return -1;
    uint64_t off = uva - c->stack_bottom;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t remaining = len, wrote = 0;
    while (remaining > 0) {
        uint64_t page = off / PAGE_SIZE, within = off % PAGE_SIZE;
        if (page >= USER_STACK_PAGES) return -1;
        uint64_t chunk = PAGE_SIZE - within;
        if (chunk > remaining) chunk = remaining;
        uint8_t *d = (uint8_t *)(DIRECTMAP_BASE + c->stack_phys[page] + within);
        for (uint64_t i = 0; i < chunk; ++i) d[i] = s[wrote + i];
        off += chunk; wrote += chunk; remaining -= chunk;
    }
    return 0;
}

/*
 * ensure_user_pagetables —— 保留占位，实际不做预创建。
 *
 * 历史问题：vmm_setup_user_space() 的第二次调用在 QEMU 中稳定返回 -1。
 * 现改为完全依赖 vmm_map_page() 的按需分配。
 */
static int ensure_user_pagetables(uint64_t *pml4) {
    if (!pml4) return -1;
    return 0;
}

/*
 * setup_user_memory —— 建立用户上下文的内存映射。
 *
 * 映射内容：
 *   1) 用户栈 [USER_STACK_BOTTOM, USER_STACK_TOP)；
 *   2) TLS 页 @ USER_TLS_BASE（含自引用）；
 *   3) Signal trampoline 页 @ USER_SIGTRAMP_ADDR；
 *   4) 初始化 heap 指针。
 */
static int setup_user_memory(struct task_t *t, struct user_ctx *c) {
    (void)t;

    if (!c || !c->pml4) return -1;

    /* 1) 用户栈 */
    for (uint64_t i = 0; i < USER_STACK_PAGES; ++i) {
        struct page *pg = pmm_alloc_pages(0);
        if (!pg) {
            serial_printf("[USER] setup_user_memory: stack pmm_alloc "
                          "failed at page %llu\n",
                          (unsigned long long)i);
            return -1;
        }
        uint64_t pa = page_to_phys(pg);
        uint64_t va = USER_STACK_BOTTOM + i * PAGE_SIZE;

        int rc = vmm_map_page(c->pml4, va, pa,
                              PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        if (rc != 0) {
            serial_printf("[USER] setup_user_memory: vmm_map_page(stack) "
                          "va=0x%llx rc=%d\n",
                          (unsigned long long)va, rc);
            pmm_free_pages(pg, 0);
            return -1;
        }
        c->stack_phys[i] = pa;
    }
    c->stack_bottom = USER_STACK_BOTTOM;
    c->stack_top    = USER_STACK_TOP;

    /* 2) TLS 页（含自引用） */
    struct page *tpg = pmm_alloc_pages(0);
    if (!tpg) {
        serial_printf("[USER] setup_user_memory: TLS pmm_alloc failed\n");
        return -1;
    }
    uint64_t tpa = page_to_phys(tpg);

    int rc = vmm_map_page(c->pml4, USER_TLS_BASE, tpa,
                          PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    if (rc != 0) {
        serial_printf("[USER] setup_user_memory: vmm_map_page(TLS) "
                      "va=0x%llx rc=%d\n",
                      (unsigned long long)USER_TLS_BASE, rc);
        pmm_free_pages(tpg, 0);
        return -1;
    }
    c->tls_base = USER_TLS_BASE;
    c->tls_phys = tpa;

    {
        uint8_t *tls_page = (uint8_t *)(DIRECTMAP_BASE + tpa);
        for (uint64_t k = 0; k < PAGE_SIZE; ++k) tls_page[k] = 0;
        uint64_t self_ref = USER_TLS_BASE;
        const uint8_t *src = (const uint8_t *)&self_ref;
        for (unsigned k = 0; k < sizeof(self_ref); ++k)
            tls_page[k] = src[k];
    }

    /* 3) Signal trampoline 页 */
    struct page *spg = pmm_alloc_pages(0);
    if (!spg) {
        serial_printf("[USER] setup_user_memory: sigtramp pmm_alloc failed\n");
        return -1;
    }
    uint64_t spa = page_to_phys(spg);

    rc = vmm_map_page(c->pml4, USER_SIGTRAMP_ADDR, spa,
                      PTE_PRESENT | PTE_USER);
    if (rc != 0) {
        serial_printf("[USER] setup_user_memory: vmm_map_page(sigtramp) "
                      "va=0x%llx rc=%d\n",
                      (unsigned long long)USER_SIGTRAMP_ADDR, rc);
        pmm_free_pages(spg, 0);
        return -1;
    }

    /*
     * 信号返回 trampoline（offset 0x00）：
     *   movq $0x133, %rax   ; SYS_OB_SigReturn
     *   syscall
     *   movq $0x130, %rax   ; 兜底：SYS_OB_UserExit
     *   xorq %rdi, %rdi
     *   syscall
     *   hlt
     *   jmp $-2
     */
    static const uint8_t tramp_code[] = {
        0x48, 0xC7, 0xC0, 0x33, 0x01, 0x00, 0x00,   /* movq $0x133, %rax */
        0x0F, 0x05,                                  /* syscall */
        0x48, 0xC7, 0xC0, 0x30, 0x01, 0x00, 0x00,   /* movq $0x130, %rax */
        0x48, 0x31, 0xFF,                            /* xorq %rdi, %rdi */
        0x0F, 0x05,                                  /* syscall */
        0xF4,                                        /* hlt */
        0xEB, 0xFE                                   /* jmp $-2 */
    };

    /*
     * ★ 任务 4 新增：pthread 线程退出 trampoline（offset 0x20）。
     *
     *   pthread 线程的用户函数在 ret 时弹出 [rsp] 处的返回地址
     *   （由 user_thread_entry 布置为 USER_THREAD_EXIT_TRAMP_ADDR），
     *   跳到此处执行。
     *
     *   MS ABI 下 void* fn(void*) 的返回值在 RAX；本 stub 把 RAX
     *   当作 int exit_code 传给 SYS_OB_UserExit(rdi)，从而优雅终止
     *   该线程并让 pthread_join 拿到返回值。
     *
     *   movq %rax, %rdi        ; exit_code = fn 的返回值（低 32 位）
     *   movq $0x130, %rax      ; SYS_OB_UserExit
     *   syscall
     *   hlt
     *   jmp $-2
     */
    static const uint8_t thread_exit_code[] = {
        0x48, 0x89, 0xC7,                            /* movq %rax, %rdi */
        0x48, 0xC7, 0xC0, 0x30, 0x01, 0x00, 0x00,   /* movq $0x130, %rax */
        0x0F, 0x05,                                  /* syscall */
        0xF4,                                        /* hlt */
        0xEB, 0xFE                                   /* jmp $-2 */
    };

    {
        uint8_t *sdst = (uint8_t *)(DIRECTMAP_BASE + spa);
        for (uint64_t i = 0; i < PAGE_SIZE; ++i) sdst[i] = 0;

        /* offset 0x00：信号返回 trampoline */
        for (uint64_t i = 0; i < sizeof(tramp_code); ++i)
            sdst[i] = tramp_code[i];

        /* offset 0x20：线程退出 trampoline */
        const uint64_t THREAD_EXIT_OFF = 0x20;
        for (uint64_t i = 0; i < sizeof(thread_exit_code); ++i)
            sdst[THREAD_EXIT_OFF + i] = thread_exit_code[i];
    }

    serial_printf("[USER] setup_user_memory: sigtramp=0x%llx "
                  "thread_exit_tramp=0x%llx\n",
                  (unsigned long long)USER_SIGTRAMP_ADDR,
                  (unsigned long long)USER_THREAD_EXIT_TRAMP_ADDR);

    /* 4) 堆指针 */
    c->heap_cur = USER_IMAGE_BASE + USER_IMAGE_MAX / 2;
    c->heap_end = USER_IMAGE_BASE + USER_IMAGE_MAX;
    c->heap_page_count = 0;

    serial_printf("[USER] setup_user_memory OK: stack=[0x%llx,0x%llx) "
                  "pages=%u tls=0x%llx(pa=0x%llx) sigtramp=0x%llx "
                  "heap=[0x%llx,0x%llx)\n",
                  (unsigned long long)c->stack_bottom,
                  (unsigned long long)c->stack_top,
                  (unsigned)USER_STACK_PAGES,
                  (unsigned long long)c->tls_base,
                  (unsigned long long)c->tls_phys,
                  (unsigned long long)USER_SIGTRAMP_ADDR,
                  (unsigned long long)c->heap_cur,
                  (unsigned long long)c->heap_end);
    return 0;
}

/*
 * free_user_pages —— 释放用户上下文的全部数据页。
 *
 * ★★★ 修复 2：接受 task_t* 参数，同步更新 mem_pages_used ★★★
 *
 * 人工必须审查：
 *   - 每释放一个用户数据页，mem_pages_used--；
 *   - 页表页、内核栈、PG_RESERVED、PG_SLAB 的页不计入配额，
 *     因此也不递减 mem_pages_used；
 *   - 若 mem_pages_used 已为 0，则不再递减（防御性）。
 */
static void free_user_pages(struct task_t *t, struct user_ctx *c)
{
    if (!c || !c->pml4) return;

    uint64_t *pml4 = c->pml4;
    uint64_t freed_data = 0, freed_pt_skipped = 0, skipped_reserved = 0;

    const uint64_t PHYS_LO = 0x100000ULL;
    const uint64_t PHYS_HI = ((uint64_t)MAX_PAGES << PAGE_SHIFT);

    for (int i4 = 1; i4 < 256; ++i4) {
        uint64_t pml4e = pml4[i4];
        if (!(pml4e & PTE_PRESENT)) continue;
        if (!(pml4e & PTE_USER)) continue;

        uint64_t pdpt_pa = pml4e & PTE_ADDR_MASK;
        if (pdpt_pa == 0) continue;
        if (!vmm_is_pt_page_strict_pub(pdpt_pa)) continue;

        uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE + pdpt_pa);
        for (int i3 = 0; i3 < 512; ++i3) {
            uint64_t pdpte = pdpt[i3];
            if (!(pdpte & PTE_PRESENT)) continue;
            if (pdpte & PTE_HUGE) continue;
            if (!(pdpte & PTE_USER)) continue;

            uint64_t pd_pa = pdpte & PTE_ADDR_MASK;
            if (pd_pa == 0) continue;
            if (!vmm_is_pt_page_strict_pub(pd_pa)) continue;

            uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE + pd_pa);
            for (int i2 = 0; i2 < 512; ++i2) {
                uint64_t pde = pd[i2];
                if (!(pde & PTE_PRESENT)) continue;
                if (pde & PTE_HUGE) continue;
                if (!(pde & PTE_USER)) continue;

                uint64_t pt_pa = pde & PTE_ADDR_MASK;
                if (pt_pa == 0) continue;
                if (!vmm_is_pt_page_strict_pub(pt_pa)) continue;

                uint64_t *pt = (uint64_t *)(DIRECTMAP_BASE + pt_pa);
                for (int i1 = 0; i1 < 512; ++i1) {
                    uint64_t pte = pt[i1];
                    if (!(pte & PTE_PRESENT)) continue;
                    if (!(pte & PTE_USER)) continue;

                    uint64_t pa = pte & PTE_ADDR_MASK;
                    pt[i1] = 0;

                    if (pa == 0) continue;
                    if (pa < PHYS_LO || pa >= PHYS_HI) { skipped_reserved++; continue; }
                    if (pa & 0xFFFULL) { skipped_reserved++; continue; }
                    if (vmm_is_pt_page_strict_pub(pa)) { freed_pt_skipped++; continue; }

                    struct page *pg = phys_to_page(pa);
                    if (pg < mem_map || pg >= mem_map + MAX_PAGES) {
                        skipped_reserved++; continue;
                    }
                    if (pg->flags & PG_RESERVED) { skipped_reserved++; continue; }
                    if (pg->flags & PG_SLAB) { skipped_reserved++; continue; }

                    pmm_free_pages(pg, 0);
                    freed_data++;

                    /* ★ 修复 2：配额回收（仅统计用户数据页） */
                    if (t && t->mem_pages_used > 0) t->mem_pages_used--;
                }
            }
        }
    }
    serial_printf("[USER] free_user_pages: freed=%llu, pt-skipped=%llu, "
                  "reserved-skipped=%llu\n",
                  (unsigned long long)freed_data,
                  (unsigned long long)freed_pt_skipped,
                  (unsigned long long)skipped_reserved);
}

int user_setup(struct task_t *t, uint64_t entry_va) {
    if (!t) return -1;
    struct user_ctx *c = ctx_alloc(t);
    if (!c) return -1;
    c->entry = entry_va;
    c->pml4 = vmm_create_address_space();
    if (!c->pml4) { ctx_free(t); return -1; }

    if (ensure_user_pagetables(c->pml4) != 0) {
        vmm_free_user_pagetables(c->pml4);
        vmm_destroy_address_space(c->pml4);
        c->pml4 = 0;
        ctx_free(t);
        return -1;
    }

    if (setup_user_memory(t, c) != 0) {
        serial_printf("[USER] setup_user_memory failed pid=%llu\n",
                      (unsigned long long)t->pid);
        free_user_pages(t, c);
        vmm_free_user_pagetables(c->pml4);
        vmm_destroy_address_space(c->pml4);
        c->pml4 = 0;
        ctx_free(t);
        return -1;
    }

    /* ★ 第 18C 步：标记主线程持有 PML4 */
    c->owns_pml4         = 1;
    c->is_thread         = 0;
    c->thread_stack_base = 0;
    c->thread_stack_pages = 0;
    c->thread_tls_phys   = 0;

    signal_init_task(t);
    serial_printf("[USER] setup pid=%llu entry=0x%llx stack=0x%llx pml4=0x%llx\n",
                  (unsigned long long)t->pid, (unsigned long long)entry_va,
                  (unsigned long long)c->stack_top,
                  (unsigned long long)(uintptr_t)c->pml4);
    return 0;
}

void user_teardown(struct task_t *t) {
    if (!t) return;
    struct user_ctx *c = user_get_ctx(t);
    if (!c) return;

    uint64_t *kpml4 = vmm_kernel_pml4();
    if (kpml4 && c->pml4 && c->pml4 != kpml4) {
        vmm_switch_address_space(kpml4);
    }

    /*
     * ★ 任务 4 修复：
     *   pthread 线程只释放"本线程独占"的页（owned 数组 + TLS 页），
     *   复用父进程 heap 页（未记入 owned 数组）不释放。
     *
     *   owned 页对应的 va 通过 cc->stack_bottom + i*PAGE_SIZE 遍历 PTE
     *   反查——因为 owned 数组记录的是物理地址，而 PTE 才是权威。
     *   一页物理地址在 PTE 中最多出现一次（同线程栈），因此遍历
     *   USER_STACK_PAGES 个页即可。
     */
    if (c->is_thread) {
    if (c->pml4) {
        for (uint32_t i = 0; i < c->thread_stack_owned_count; ++i) {
            uint64_t pa = c->thread_stack_owned_phys[i];
            if (pa == 0) continue;

            /* 清对应 PTE */
            for (uint32_t p = 0; p < USER_STACK_PAGES; ++p) {
                uint64_t va = c->stack_bottom + (uint64_t)p * PAGE_SIZE;
                uint64_t *pte = vmm_get_pte(c->pml4, va);
                if (pte && (*pte & PTE_PRESENT) &&
                    ((*pte & PTE_ADDR_MASK) == pa)) {
                    *pte = 0;
                    break;
                }
            }

            struct page *pg = phys_to_page(pa);
            if (pg && !(pg->flags & PG_RESERVED) &&
                !(pg->flags & PG_SLAB)) {
                pmm_free_pages(pg, 0);
                /* ★ 修复 2：配额回收（own 栈页） */
                if (t && t->mem_pages_used > 0) t->mem_pages_used--;
            }
        }

        if (c->thread_tls_phys) {
            uint64_t *pte = vmm_get_pte(c->pml4, c->tls_base);
            if (pte) *pte = 0;
            struct page *pg = phys_to_page(c->thread_tls_phys);
            if (pg && !(pg->flags & PG_RESERVED) &&
                !(pg->flags & PG_SLAB)) {
                pmm_free_pages(pg, 0);
                /* ★ 修复 2：配额回收（TLS 页） */
                if (t && t->mem_pages_used > 0) t->mem_pages_used--;
            }
        }
        vmm_flush_tlb();
    }

    } else {
        if (c->pml4) {
            free_user_pages(t, c);       /* ★ 修复 2：传入 task_t */
            vmm_free_user_pagetables(c->pml4);
            vmm_destroy_address_space(c->pml4);
            c->pml4 = 0;
        }
    }

    signal_free_task(t);
    ctx_free(t);
}

static uint64_t user_build_argv(struct user_ctx *c, int argc, const char **argv) {
    if (argc > 16) argc = 16;
    if (argc < 0) argc = 0;
    uint64_t sp = c->stack_top;
    uint64_t arg_ptrs[16];
    for (int i = argc - 1; i >= 0; --i) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        uint64_t len = 0; while (s[len] && len < 511) ++len; ++len;
        sp -= len; sp &= ~0xFULL;
        user_stack_write(c, sp, s, len);
        arg_ptrs[i] = sp;
    }
    sp -= (uint64_t)(argc + 1) * 8; sp &= ~0xFULL;
    for (int i = 0; i < argc; ++i) {
        uint64_t v = arg_ptrs[i]; user_stack_write(c, sp + i * 8, &v, 8);
    }
    uint64_t null_ptr = 0;
    user_stack_write(c, sp + argc * 8, &null_ptr, 8);
    sp -= 16; sp &= ~0xFULL;
    uint64_t hdr[2] = { sp + 16, (uint64_t)argc };
    user_stack_write(c, sp, hdr, 16);
    return sp;
}

static void dump_pte_path(uint64_t *pml4, uint64_t va, const char *tag)
{
    uint64_t i4 = PML4_INDEX(va);
    uint64_t i3 = PDPT_INDEX(va);
    uint64_t i2 = PD_INDEX(va);
    uint64_t i1 = PT_INDEX(va);

    uint64_t pml4e = pml4[i4];
    uint64_t pdpte = 0, pde = 0, pte = 0;
    uint64_t pdpt_pa = 0, pd_pa = 0, pt_pa = 0;

    if (pml4e & PTE_PRESENT) {
        pdpt_pa = pml4e & PTE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE + pdpt_pa);
        pdpte = pdpt[i3];
        if (pdpte & PTE_PRESENT) {
            pd_pa = pdpte & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE + pd_pa);
            pde = pd[i2];
            if ((pde & PTE_PRESENT) && !(pde & PTE_HUGE)) {
                pt_pa = pde & PTE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)(DIRECTMAP_BASE + pt_pa);
                pte = pt[i1];
            }
        }
    }

    serial_printf("[PTE-DUMP %s] va=0x%llx\n", tag, (unsigned long long)va);
    serial_printf("  i4=%llu i3=%llu i2=%llu i1=%llu\n",
                  (unsigned long long)i4, (unsigned long long)i3,
                  (unsigned long long)i2, (unsigned long long)i1);
    serial_printf("  pml4e=0x%llx (P=%llu U=%llu)\n",
                  (unsigned long long)pml4e,
                  (unsigned long long)(pml4e & 1),
                  (unsigned long long)((pml4e >> 2) & 1));
    serial_printf("  pdpt_pa=0x%llx pdpte=0x%llx\n",
                  (unsigned long long)pdpt_pa, (unsigned long long)pdpte);
    serial_printf("  pd_pa=0x%llx pde=0x%llx\n",
                  (unsigned long long)pd_pa, (unsigned long long)pde);
    serial_printf("  pt_pa=0x%llx pte[%llu]=0x%llx\n",
                  (unsigned long long)pt_pa,
                  (unsigned long long)i1, (unsigned long long)pte);
    serial_printf("  present=%llu writable=%llu user=%llu nx=%llu\n",
                  (unsigned long long)(pte & 1),
                  (unsigned long long)((pte >> 1) & 1),
                  (unsigned long long)((pte >> 2) & 1),
                  (unsigned long long)((pte >> 63) & 1));
}

void user_enter(uint64_t entry, uint64_t user_rsp, int argc, const char **argv)
{
    serial_puts("[ENTER-A] enter user_enter\n");

    struct task_t *t = task_from_thread(sched_current());
    if (!t) { serial_puts("[ENTER-B] no task -> halt\n"); for (;;) __asm__ __volatile__("hlt"); }
    serial_puts("[ENTER-C] got task\n");

    struct user_ctx *c = user_get_ctx(t);
    if (!c) { serial_puts("[ENTER-D] no ctx -> halt\n"); for (;;) __asm__ __volatile__("hlt"); }
    serial_printf("[ENTER-E] ctx pml4=0x%llx tls=0x%llx\n",
                  (unsigned long long)(uintptr_t)c->pml4,
                  (unsigned long long)c->tls_base);

    {
        extern uint64_t g_syscall_kernel_rsp;
        extern uint64_t g_syscall_user_rsp;
        struct thread *th = sched_current();
        if (th && th->kstack_base) {
            uint64_t ktop = (uint64_t)th->kstack_base + th->kstack_size;
            g_syscall_kernel_rsp = ktop & ~0xFULL;
        } else { g_syscall_kernel_rsp = 0; }
        g_syscall_user_rsp = 0;
        serial_printf("[ENTER-F] set g_syscall_kernel_rsp=0x%llx\n",
                      (unsigned long long)g_syscall_kernel_rsp);
    }

    if (argc > 0 && argv) {
        user_rsp = user_build_argv(c, argc, argv);
    } else {
        user_rsp = (c->stack_top - 16) & ~0xFULL;
        uint64_t hdr[2] = { 0, 0 };
        user_stack_write(c, user_rsp, hdr, 16);
    }
    serial_printf("[ENTER-G] user_rsp=0x%llx\n", (unsigned long long)user_rsp);

    serial_puts("[ENTER-H] before set_fsbase\n");
    user_set_fsbase(c->tls_base);

    serial_puts("[ENTER-I] before switch_address_space\n");
    {
        uint64_t cr3_now;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_now));
        serial_printf("[ENTER-I-0] CR3 now=0x%llx\n", (unsigned long long)cr3_now);
        uint64_t *k_pml4 = vmm_kernel_pml4();
        serial_printf("[ENTER-I-1] kernel_pml4=0x%llx\n", (unsigned long long)(uintptr_t)k_pml4);
        if (k_pml4) {
            serial_printf("[ENTER-I-2] k[0]=0x%llx k[256]=0x%llx k[511]=0x%llx\n",
                          (unsigned long long)k_pml4[0], (unsigned long long)k_pml4[256],
                          (unsigned long long)k_pml4[511]);
        }
        serial_printf("[ENTER-I-3] user_pml4=0x%llx\n", (unsigned long long)(uintptr_t)c->pml4);
        serial_printf("[ENTER-I-4] u[0]=0x%llx u[1]=0x%llx u[255]=0x%llx\n",
                      (unsigned long long)c->pml4[0], (unsigned long long)c->pml4[1],
                      (unsigned long long)c->pml4[255]);
        serial_printf("[ENTER-I-5] u[256]=0x%llx u[511]=0x%llx\n",
                      (unsigned long long)c->pml4[256], (unsigned long long)c->pml4[511]);
    }

    dump_pte_path(c->pml4, entry, "pre-switch");

    uint64_t rsp = user_rsp & ~0xFULL;
    uint64_t rip = entry;

    serial_printf("[USER-ENTER] rip=0x%llx rsp=0x%llx cr3=0x%llx "
                  "kernel_rsp=0x%llx tls=0x%llx\n",
                  (unsigned long long)rip, (unsigned long long)rsp,
                  (unsigned long long)((uint64_t)c->pml4 - DIRECTMAP_BASE),
                  (unsigned long long)g_syscall_kernel_rsp,
                  (unsigned long long)c->tls_base);

    vmm_switch_address_space(c->pml4);
    serial_puts("[ENTER-J] CR3 switched\n");

    dump_pte_path(c->pml4, entry, "post-switch");

    serial_puts("[USER-ENTER] iretq -> CPL=3 now\n");

    __asm__ __volatile__(
        "cli\n\t"
        "clac\n\t"
        "mov $0x1B, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "push $0x1B\n\t"
        "push %[rsp]\n\t"
        "push $0x202\n\t"
        "push $0x23\n\t"
        "push %[rip]\n\t"
        "iretq\n\t"
        :
        : [rsp] "r"(rsp), [rip] "r"(rip)
        : "rax", "memory");
    for (;;) { __asm__ __volatile__("hlt"); }
}

struct user_regs *signal_resume_slot(struct task_t *t);

/* ★ 第 18C 步：以下两个 slot 函数由 signal.c 提供 */
struct user_regs *signal_resume_slot(struct task_t *t);
struct user_regs *signal_handler_slot(struct task_t *t);

/* ★ 第 18C 步：user_resume_asm 由 user_resume.S 提供 */
extern void user_resume_asm(struct user_regs *r);

/*
 * user_resume_ctx —— 从内核态 iretq 回用户态（使用指定上下文）。
 *
 * 参数 r 由调用者选择：
 *   - idt.c 的异常处理路径传入 signal_handler_slot(cur)，
 *     即"进入 handler"的上下文；
 *   - syscall.c 的 SYS_OB_SigReturn 传入 signal_resume_slot(cur)，
 *     即"sigreturn 恢复"的原始上下文。
 *
 * ★ 第 18C 步修复：原实现用 C inline asm 加载 20 个寄存器，但
 *   clobber list 只声明了 "memory"，导致编译器可能把 r 的基址放在
 *   被 asm 修改的寄存器中，触发 SMAP #PF。现在委托给独立汇编函数
 *   user_resume_asm（见 user_resume.S）。
 */
void user_resume_ctx(struct task_t *t, struct user_regs *r)
{
    if (!r) {
        serial_printf("[USER] resume: no saved regs pid=%llu\n",
                      (unsigned long long)(t ? t->pid : 0));
        task_exit(-1);
    }
    struct user_ctx *c = user_get_ctx(t);
    if (!c) task_exit(-1);
    user_set_fsbase(c->tls_base);
    vmm_switch_address_space(c->pml4);
    user_resume_asm(r);
    /* not reached */
    for (;;) __asm__ __volatile__("hlt");
}

/* 兼容旧接口：默认使用"原始上下文"（sigreturn 语义）。 */
void user_resume(struct task_t *t)
{
    user_resume_ctx(t, signal_resume_slot(t));
}

int user_spawn_program(const char *path)
{
    serial_printf("[USER-DBG] spawn: enter path=%s\n",
                  path ? path : "(null)");
    if (!path) return -1;

    struct thread *th = sched_current();
    struct task_t *t  = task_from_thread(th);
    if (!t || !th) {
        serial_printf("[USER-DBG] spawn: no current task\n");
        return -1;
    }
    serial_printf("[USER-DBG] spawn: pid=%llu\n",
                  (unsigned long long)t->pid);

    if (t->user_ctx) {
        serial_printf("[USER] spawn: pid=%llu already has user_ctx\n",
                      (unsigned long long)t->pid);
        return -1;
    }

    struct vfs_inode *ino = 0;
    int rc = vfs_lookup(path, &ino);
    if (rc != 0 || !ino) {
        serial_printf("[USER] spawn: lookup '%s' failed rc=%d\n", path, rc);
        return -1;
    }
    uint64_t fsize = ino->size;
    serial_printf("[USER-DBG] spawn: fsize=%llu\n",
                  (unsigned long long)fsize);
    if (fsize == 0) {
        serial_printf("[USER] spawn: '%s' empty\n", path);
        return -1;
    }
    if (fsize > USER_IMAGE_MAX) {
        serial_printf("[USER] spawn: '%s' too large\n", path);
        return -1;
    }

    uint64_t npages = (fsize + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while (((uint64_t)1 << order) < npages) order++;
    if (order >= MAX_ORDER) return -1;
    serial_printf("[USER-DBG] spawn: npages=%llu order=%d\n",
                  (unsigned long long)npages, order);

    struct page *img_pg = pmm_alloc_pages(order);
    if (!img_pg) return -1;
    uint64_t img_phys = page_to_phys(img_pg);
    uint8_t *img = (uint8_t *)(DIRECTMAP_BASE + img_phys);
    serial_printf("[USER-DBG] spawn: img phys=0x%llx va=%p\n",
                  (unsigned long long)img_phys, (void *)img);

    struct vfs_file *f = 0;
    rc = vfs_open(path, VFS_O_RDONLY, &f);
    if (rc != 0 || !f) { pmm_free_pages(img_pg, order); return -1; }
    int64_t total = vfs_read(f, img, fsize);
    vfs_close(f);
    if (total != (int64_t)fsize) {
        pmm_free_pages(img_pg, order);
        return -1;
    }

    const struct obr_header *h = (const struct obr_header *)img;
    if (h->magic != OBR_MAGIC || h->arch != OBR_ARCH_X64) {
        serial_printf("[USER] spawn: '%s' not a valid x64 .obr\n", path);
        pmm_free_pages(img_pg, order);
        return -1;
    }
    serial_printf("[USER-DBG] spawn: obr entry_point=0x%llx ph_count=%u "
                  "dep_count=%llu\n",
                  (unsigned long long)h->entry_point,
                  (unsigned)h->ph_count,
                  (unsigned long long)h->dep_count);

    uint64_t entry_placeholder = USER_IMAGE_BASE + h->entry_point;
    int urc = user_setup(t, entry_placeholder);
    if (urc != 0) {
        pmm_free_pages(img_pg, order);
        return urc;
    }
    struct user_ctx *c = user_get_ctx(t);
    if (!c || !c->pml4) { pmm_free_pages(img_pg, order); return -1; }

    uint64_t ph_off = h->ph_offset;
    if (ph_off + (uint64_t)h->ph_count * sizeof(struct obr_phdr) > fsize) {
        pmm_free_pages(img_pg, order);
        return -1;
    }

    for (uint16_t i = 0; i < h->ph_count; ++i) {
        const struct obr_phdr *ph = (const struct obr_phdr *)
            (img + ph_off + (uint64_t)i * sizeof(struct obr_phdr));

        serial_printf("[USER-DBG]   ph[%u] type=%u flags=0x%x "
                      "offset=0x%llx vaddr=0x%llx filesz=0x%llx memsz=0x%llx\n",
                      (unsigned)i, (unsigned)ph->type, (unsigned)ph->flags,
                      (unsigned long long)ph->offset,
                      (unsigned long long)ph->vaddr,
                      (unsigned long long)ph->filesz,
                      (unsigned long long)ph->memsz);

        if (ph->type != OBR_PT_LOAD) continue;
        if (ph->offset + ph->filesz > fsize) {
            pmm_free_pages(img_pg, order); return -1;
        }
        if (ph->filesz > ph->memsz) {
            pmm_free_pages(img_pg, order); return -1;
        }

        uint64_t dst_va = USER_IMAGE_BASE + ph->vaddr;
        uint64_t nseg_pages = (ph->memsz + PAGE_SIZE - 1) / PAGE_SIZE;
        if (nseg_pages == 0) continue;

        serial_printf("[USER-DBG]   ph[%u] -> dst_va=0x%llx npages=%llu\n",
                      (unsigned)i, (unsigned long long)dst_va,
                      (unsigned long long)nseg_pages);

        for (uint64_t p = 0; p < nseg_pages; ++p) {
            struct page *pg = pmm_alloc_pages(0);
            if (!pg) { pmm_free_pages(img_pg, order); return -1; }
            uint64_t pa = page_to_phys(pg);
            uint64_t va = dst_va + p * PAGE_SIZE;

            int mrc = vmm_map_page(c->pml4, va, pa,
                                   PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            if (mrc != 0) {
                serial_printf("[USER] spawn: vmm_map_page va=0x%llx rc=%d\n",
                              (unsigned long long)va, mrc);
                pmm_free_pages(pg, 0);
                pmm_free_pages(img_pg, order);
                return -1;
            }

            uint64_t *chk = vmm_get_pte(c->pml4, va);
            serial_printf("[USER-DBG] map[%u][%llu] va=0x%llx pa=0x%llx "
                          "pte_ptr=%p pte_val=0x%llx\n",
                          (unsigned)i, (unsigned long long)p,
                          (unsigned long long)va, (unsigned long long)pa,
                          (void *)chk,
                          (unsigned long long)(chk ? *chk : 0));
            if (!chk || !(*chk & PTE_PRESENT)) {
                serial_printf("[USER] FATAL: pte missing immediately after map!\n");
                pmm_free_pages(img_pg, order);
                return -1;
            }
        }

        for (uint64_t p = 0; p < nseg_pages; ++p) {
            uint64_t va = dst_va + p * PAGE_SIZE;
            uint64_t *pte = vmm_get_pte(c->pml4, va);
            if (!pte || !(*pte & PTE_PRESENT)) {
                serial_printf("[USER] spawn: pte vanished for 0x%llx\n",
                              (unsigned long long)va);
                pmm_free_pages(img_pg, order);
                return -1;
            }
            uint64_t pa = *pte & PTE_ADDR_MASK;
            uint8_t *dst = (uint8_t *)(DIRECTMAP_BASE + pa);
            uint64_t base = p * PAGE_SIZE, end = base + PAGE_SIZE;
            for (uint64_t k = base; k < end; ++k) {
                if (k < ph->filesz) dst[k - base] = img[ph->offset + k];
                else                dst[k - base] = 0;
            }
        }
    }

    uint64_t entry_va = USER_IMAGE_BASE;
    for (uint16_t i = 0; i < h->ph_count; ++i) {
        const struct obr_phdr *ph = (const struct obr_phdr *)
            (img + ph_off + (uint64_t)i * sizeof(struct obr_phdr));
        if (ph->type != OBR_PT_LOAD) continue;
        if (h->entry_point >= ph->offset &&
            h->entry_point < ph->offset + ph->filesz) {
            uint64_t in_seg = h->entry_point - ph->offset;
            entry_va = USER_IMAGE_BASE + ph->vaddr + in_seg;
            serial_printf("[USER-DBG] spawn: entry_point=0x%llx maps to seg %u "
                          "in_seg=0x%llx entry_va=0x%llx\n",
                          (unsigned long long)h->entry_point, (unsigned)i,
                          (unsigned long long)in_seg,
                          (unsigned long long)entry_va);
            break;
        }
    }

    serial_printf("[USER] spawn '%s': entry_va=0x%llx pid=%llu "
                  "size=%llu (CPL=3 next)\n",
                  path, (unsigned long long)entry_va,
                  (unsigned long long)t->pid,
                  (unsigned long long)fsize);

    dump_pte_path(c->pml4, entry_va, "before-free-img");

    /* ★ 修复 A：先处理动态依赖，再释放 img_pg。
     *
     * 人工必须审查：
     *   - deps_blob 指向 img_pg 内部；若 img_pg 先被释放，
     *     pmm_alloc_pages 可能立即复用同一物理页，
     *     vfs_read 覆写 deps_blob 内容，导致库名乱码。
     *   - 正确的顺序：elf_load_deps 完成 -> 再 pmm_free_pages(img_pg)。 */
    if (h->dep_count > 0 && h->dep_strings_offset != 0 &&
        h->dep_strings_offset < fsize) {
        const char *deps = (const char *)(img + h->dep_strings_offset);
        serial_printf("[USER] spawn: resolving %llu dynamic deps\n",
                      (unsigned long long)h->dep_count);
        int drc = elf_load_deps(c->pml4, deps, (uint32_t)h->dep_count);
        if (drc != 0) {
            serial_printf("[USER] spawn: elf_load_deps rc=%d\n", drc);
        }
    }

    serial_puts("[SPAWN-A] before free img_pg\n");
    pmm_free_pages(img_pg, order);
    serial_puts("[SPAWN-B] after free img_pg\n");

    dump_pte_path(c->pml4, entry_va, "after-free-img");

    serial_puts("[SPAWN-C] calling user_enter\n");
    user_enter(entry_va, c->stack_top, 0, 0);
    serial_puts("[SPAWN-D] user_enter returned (should not happen)\n");
    return 0;
}

void user_launcher_entry(void *arg)
{
    const char *path = (const char *)arg;
    struct task_t *self = task_from_thread(sched_current());
    serial_printf("[USER] launcher: pid=%llu path='%s'\n",
                  (unsigned long long)(self ? self->pid : 0), path ? path : "(null)");
    int rc = user_spawn_program(path);
    if (rc != 0) {
        serial_printf("[USER] launcher: user_spawn_program failed rc=%d\n", rc);
        task_exit(-1);
    }
    for (;;) __asm__ __volatile__("hlt");
}

static const char *const g_user_test_paths[] = {
    "/bin/syscall_test.obr",
    "/bin/hello.obr",
    "/bin/fs_test.obr",
    "/bin/exception_test.obr",
    "/bin/signal_test.obr",
    "/bin/pthread_test.obr",
    "/bin/dyn_test.obr",
    "/bin/pipe_test.obr",   /* ★ 第 18D 步：pipe 用户态验收 */
    0
};

static void user_test_child_entry(void *arg)
{
    const char *path = (const char *)arg;
    struct task_t *self = task_from_thread(sched_current());
    serial_printf("[USER] child: pid=%llu path='%s'\n",
                  (unsigned long long)(self ? self->pid : 0),
                  path ? path : "(null)");

    int rc = user_spawn_program(path);
    if (rc != 0) {
        serial_printf("[USER] child: spawn '%s' failed rc=%d\n",
                      path ? path : "(null)", rc);
        task_exit(-1);
    }
    for (;;) __asm__ __volatile__("hlt");
}

/*
 * user_test_driver_entry —— 顺序启动用户态测试程序并等待其结束。
 *
 * ★ 第 18D 步修复：
 *   原 18C 协议使用 task_find_by_pid() 判断子进程是否退出。18D 引入
 *   zombie 语义后，task_find_by_pid() 会返回已退出但未 join 的 task，
 *   导致死循环。现改为直接调用 task_join()——它既等待退出，又回收
 *   task_t 骨架，并返回真实 exit_code。
 */
void user_test_driver_entry(void *arg)
{
    (void)arg;

    struct task_t *self = task_from_thread(sched_current());
    serial_printf("[USER] driver: pid=%llu starting\n",
                  (unsigned long long)(self ? self->pid : 0));

    for (int i = 0; g_user_test_paths[i]; ++i) {
        const char *path = g_user_test_paths[i];
        serial_printf("[USER] driver: launch '%s'\n", path);

        struct task_t *child = task_create("utest",
                                           user_test_child_entry,
                                           (void *)path,
                                           0,
                                           0,
                                           5,
                                           0,
                                           0);
        if (!child) {
            serial_printf("[USER] driver: cannot create child for '%s'\n",
                          path);
            continue;
        }

        uint64_t child_pid = child->pid;
        serial_printf("[USER] driver: waiting for '%s' (pid=%llu)\n",
                      path, (unsigned long long)child_pid);

        int code = 0;
        int rc = task_join(child_pid, &code);
        if (rc != 0) {
            serial_printf("[USER] driver: task_join('%s') rc=%d\n",
                          path, rc);
        } else {
            serial_printf("[USER] driver: '%s' done code=%d\n",
                          path, code);
        }
    }

    serial_printf("[USER] driver: all tests done\n");
    task_exit(0);

    for (;;) { __asm__ __volatile__("hlt"); }
}

/* ============================================================
 * ★ 第 18C 步：pthread 线程支撑
 * ============================================================ */

/*
 * 用户线程入口：被内核通过 task_create 调度后调用。
 * 本函数在内核态执行一次，然后 iretq 进入用户态。
 */
/*
 * pthread 线程的内核态启动函数。
 *
 * 由 task_create 调度进入，在内核态执行一次，然后 iretq 进入用户态。
 *
 * ★ 任务 4 核心修复：正确布置用户态入口栈。
 *
 * MS ABI 约定用户函数 fn(arg) 的入口状态：
 *   - RCX = arg（第一个参数）
 *   - [rsp +  0] = 返回地址
 *   - [rsp +  8 .. rsp+40) = 32 字节 shadow space
 *   - rsp 16 字节对齐
 *
 * iretq 不 push 任何东西；它直接从内核栈弹出 SS/RSP/RFLAGS/CS/RIP，
 * 然后 CPU 跳转到 RIP 并设置 RSP = 弹出的值。因此必须在 iretq 之前
 * 把用户栈 [rsp+0..rsp+40) 布置为 [ret_addr | 0 | 0 | 0 | 0]，
 * 并将 RCX 设为 arg。
 *
 * 若无此布置，用户函数 ret 时会从全零栈弹出 0 → RIP=0 → #PF(I=1)
 * （症状：日志中 "RIP=0x0 CS=0x23 err=0x15"）。
 */
static void user_thread_entry(void *arg)
{
    (void)arg;   /* 参数通过 c->heap_phys[0] 传递（见 user_thread_spawn） */

    struct task_t *self = task_from_thread(sched_current());
    if (!self) { task_exit(-1); }

    struct user_ctx *c = user_get_ctx(self);
    if (!c) { task_exit(-1); }

    /* 设置 %fs 基址到本线程 TLS */
    user_set_fsbase(c->tls_base);

    /* 切换到父进程 PML4（所有 pthread 线程共享） */
    vmm_switch_address_space(c->pml4);

    uint64_t rip  = c->entry;
    uint64_t rarg = c->heap_phys[0];   /* 用户函数第一参数 */

    /*
     * 布置用户栈：
     *   [rsp +  0]        = 返回地址（指向线程退出 trampoline）
     *   [rsp +  8 .. 40)  = 32 字节 shadow space（清零）
     * rsp 16 字节对齐。
     */
    uint64_t user_rsp = c->stack_top;
    user_rsp &= ~0xFULL;
    user_rsp -= 40;                    /* 8 (ret) + 32 (shadow) */

    uint64_t ret_addr = USER_THREAD_EXIT_TRAMP_ADDR;
    if (user_stack_write(c, user_rsp + 0, &ret_addr, 8) != 0) {
        serial_printf("[USER] thread_entry: cannot write ret_addr\n");
        task_exit(-1);
    }

    uint64_t zero = 0;
    for (int i = 1; i < 5; ++i) {
        if (user_stack_write(c, user_rsp + (uint64_t)i * 8,
                             &zero, 8) != 0) {
            serial_printf("[USER] thread_entry: cannot write shadow\n");
            task_exit(-1);
        }
    }

    serial_printf("[USER] thread_entry: tid=%llu rip=0x%llx rsp=0x%llx "
                  "arg=0x%llx ret_addr=0x%llx\n",
                  (unsigned long long)self->pid,
                  (unsigned long long)rip,
                  (unsigned long long)user_rsp,
                  (unsigned long long)rarg,
                  (unsigned long long)ret_addr);

    /*
     * iretq 进入用户态。
     *   栈顺序（push 从低到高）：SS, RSP, RFLAGS, CS, RIP
     *   iretq 后：CS=0x23, RIP=rip, RFLAGS=0x202, RSP=user_rsp, SS=0x1B
     *   其余通用寄存器（含 RCX）保持当前内核态值：
     *     RCX 在 iretq 前被设为 rarg。
     */
    __asm__ __volatile__(
        "cli\n\t"
        "clac\n\t"
        "mov $0x1B, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %[arg], %%rcx\n\t"
        "push $0x1B\n\t"
        "push %[rsp]\n\t"
        "push $0x202\n\t"
        "push $0x23\n\t"
        "push %[rip]\n\t"
        "iretq\n\t"
        :
        : [rsp] "r"(user_rsp), [rip] "r"(rip), [arg] "r"(rarg)
        : "rax", "rcx", "memory");

    /* 不应到达 */
    for (;;) __asm__ __volatile__("hlt");
}

/*
 * user_thread_spawn —— 创建一个共享父进程地址空间的 pthread 线程。
 *
 * 参数（人工必须审查）：
 *   parent      —— 父进程 task_t（必须已有 user_ctx）；
 *   entry       —— 用户态入口函数 VA；
 *   arg         —— 传给 entry 的参数（通过 RCX 传递，见 user_thread_entry）；
 *   ustack_top  —— 线程栈顶用户 VA（通常由 pthread.c 的 ob_alloc_thread_stack
 *                  通过 ob_brk 得到，已 16 字节对齐）。
 *
 * 语义：
 *   - 内核 task_t / thread 由 task_create 创建；
 *   - 子线程 user_ctx 共享父进程 PML4（owns_pml4=0, is_thread=1）；
 *   - 线程栈页：
 *       · 若 pthread.c 已通过 ob_brk 在父 PML4 中映射（PTE 存在），
 *         则复用该映射（不占 owned 数组）；
 *       · 否则由内核 pmm_alloc_pages + vmm_map_page 分配，
 *         记入 cc->thread_stack_owned_phys[]（teardown 时释放）；
 *   - TLS 页由内核分配，映射到父 PML4 的一个独立 4KB 槽位；
 *   - 所有新增页都计入父进程 mem_pages_used（配额）。
 *
 * 失败回滚（人工必须审查）：
 *   - 任何一步失败都必须：
 *       1) 释放本线程 own 的栈页与 TLS 页；
 *       2) 递减父进程 mem_pages_used；
 *       3) 调用 task_destroy(child) 释放 task_t / thread / 内核栈。
 *   - 回滚顺序：先清 PTE，再释放物理页，再递减配额。
 *
 * 返回值：
 *   成功返回新线程的全局 PID（> 0）；
 *   失败返回负错误码（OB_EAGAIN / OB_ENOMEM / OB_EIO / OB_EPERM）。
 */
int64_t user_thread_spawn(struct task_t *parent,
                          uint64_t entry, uint64_t arg,
                          uint64_t ustack_top)
{
    if (!parent) return OB_EINVAL;

    struct user_ctx *pc = user_get_ctx(parent);
    if (!pc || !pc->pml4) return OB_EPERM;

    /* 参数合法性 */
    if (!user_range_ok(entry, 1)) return OB_EFAULT;
    if (ustack_top < USER_STACK_SIZE) return OB_EFAULT;
    if (!user_range_ok(ustack_top - USER_STACK_SIZE, USER_STACK_SIZE))
        return OB_EFAULT;

    /* ------------------------------------------------------------------
     * 1) 创建内核 task_t / thread
     *
     * 人工必须审查：
     *   - task_create 会为 child 分配独立 task_t 与内核栈；
     *   - 子线程继承父进程权限等级；
     *   - 后续步骤会为 child 建立 user_ctx 并绑定到父 PML4。
     * ------------------------------------------------------------------ */
    struct task_t *child = task_create("utest-thread",
                                       user_thread_entry, (void *)0,
                                       parent,
                                       0,
                                       parent->privilege_level,
                                       0,
                                       0);
    if (!child) return OB_EAGAIN;

    /* ------------------------------------------------------------------
     * 2) 建立子线程 user_ctx
     *
     * 人工必须审查：
     *   - owns_pml4 = 0：不拥有 PML4，teardown 不释放页表；
     *   - is_thread = 1：teardown 走"只释放 own 页"分支；
     *   - heap_phys[0] = arg：把用户函数参数暂存在 ctx 中，
     *     user_thread_entry 会在 iretq 前把它装入 RCX。
     * ------------------------------------------------------------------ */
    struct user_ctx *cc = (struct user_ctx *)kzalloc(sizeof(*cc));
    if (!cc) {
        task_destroy(child);
        return OB_ENOMEM;
    }
    {
        uint8_t *p = (uint8_t *)cc;
        for (unsigned i = 0; i < sizeof(*cc); ++i) p[i] = 0;
    }

    cc->entry        = entry;
    cc->pml4         = pc->pml4;
    cc->owns_pml4    = 0;
    cc->is_thread    = 1;
    cc->heap_phys[0] = arg;

    /* ------------------------------------------------------------------
     * 3) 映射本线程用户栈
     *
     * 策略（人工必须审查）：
     *   - pthread.c 的 ob_alloc_thread_stack 已经通过 ob_brk 让内核在父进程
     *     PML4 中映射了线程栈区域。user_thread_spawn 不能再重复映射。
     *   - 若目标 va 已有 PTE（存在、USER、可写），直接复用其物理地址，
     *     记录到 cc->stack_phys[i] 供 user_thread_entry 使用；
     *     不写入 owned 数组（teardown 不释放）。
     *   - 若目标 va 尚无 PTE，pmm_alloc_pages + vmm_map_page，
     *     同时写入 owned 数组（teardown 释放），并计入父进程配额。
     * ------------------------------------------------------------------ */
    uint64_t stack_bottom = ustack_top - USER_STACK_SIZE;
    cc->stack_bottom = stack_bottom;
    cc->stack_top    = ustack_top;
    cc->thread_stack_base = stack_bottom;

    uint32_t spages = USER_STACK_PAGES;       /* 16 页 = 64KB */
    cc->thread_stack_pages = spages;
    cc->thread_stack_owned_count = 0;

    for (uint32_t i = 0; i < spages; ++i) {
        uint64_t va = stack_bottom + (uint64_t)i * PAGE_SIZE;
        uint64_t *pte = vmm_get_pte(pc->pml4, va);

        /* --- 3a) 复用父进程已有映射（典型：ob_brk heap 页） --- */
        if (pte && (*pte & PTE_PRESENT) && (*pte & PTE_USER)) {
            cc->stack_phys[i] = *pte & PTE_ADDR_MASK;
            continue;
        }

        /* --- 3b) 新分配页：先检查配额 --- */
        if (user_quota_alloc(parent, 1) != 0) {
            goto rollback_stack;
        }

        struct page *pg = pmm_alloc_pages(0);
        if (!pg) {
            goto rollback_stack;
        }

        uint64_t pa = page_to_phys(pg);
        int rc = vmm_map_page(pc->pml4, va, pa,
                              PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        if (rc != 0) {
            pmm_free_pages(pg, 0);
            goto rollback_stack;
        }

        cc->stack_phys[i] = pa;
        cc->thread_stack_owned_phys[cc->thread_stack_owned_count++] = pa;

        /* 配额累加：仅对新分配的页计数 */
        user_quota_commit(parent, 1);
    }

    /* ------------------------------------------------------------------
     * 4) 映射 TLS 页
     *
     * 人工必须审查：
     *   - 子线程 TLS VA = 父进程 tls_base + N*4KB，N 由 child->pid 决定，
     *     避免与父线程 TLS 页冲突；
     *   - TLS 页前 8 字节写入 child_tls_va 自身（自引用），
     *     使 pthread.c 的 tls_self() 通过 %fs:0 读到 tls_block*；
     *   - TLS 页计入父进程配额。
     * ------------------------------------------------------------------ */
    uint64_t child_tls_va = pc->tls_base + 0x1000ULL *
                            ((uint64_t)(child->pid % 16) + 1ULL);

    /* TLS VA 冲突检查（防御性） */
    {
        uint64_t *exist = vmm_get_pte(pc->pml4, child_tls_va);
        if (exist && (*exist & PTE_PRESENT)) {
            serial_printf("[USER] thread_spawn: TLS VA 0x%llx already mapped\n",
                          (unsigned long long)child_tls_va);
            goto rollback_stack;
        }
    }

    if (user_quota_alloc(parent, 1) != 0) {
        goto rollback_stack;
    }

    struct page *tpg = pmm_alloc_pages(0);
    if (!tpg) {
        goto rollback_stack;
    }

    uint64_t tpa = page_to_phys(tpg);
    int trc = vmm_map_page(pc->pml4, child_tls_va, tpa,
                           PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    if (trc != 0) {
        pmm_free_pages(tpg, 0);
        goto rollback_stack;
    }

    cc->tls_base        = child_tls_va;
    cc->tls_phys        = tpa;
    cc->thread_tls_phys = tpa;

    /* 清零 TLS 页并写入自引用 */
    {
        uint8_t *tp = (uint8_t *)(DIRECTMAP_BASE + tpa);
        for (uint64_t k = 0; k < PAGE_SIZE; ++k) tp[k] = 0;
        uint64_t self_ref = child_tls_va;
        const uint8_t *src = (const uint8_t *)&self_ref;
        for (unsigned k = 0; k < sizeof(self_ref); ++k)
            tp[k] = src[k];
    }

    user_quota_commit(parent, 1);

    /* ------------------------------------------------------------------
     * 5) 交换子任务 user_ctx
     * ------------------------------------------------------------------ */
    child->user_ctx = cc;

    /* ------------------------------------------------------------------
     * 6) 初始化信号状态
     * ------------------------------------------------------------------ */
    signal_init_task(child);

    serial_printf("[USER] thread spawned tid=%llu entry=0x%llx "
                  "stack=0x%llx..0x%llx tls=0x%llx "
                  "owned_pages=%u reused_pages=%u parent_used=%u/%u\n",
                  (unsigned long long)child->pid,
                  (unsigned long long)entry,
                  (unsigned long long)stack_bottom,
                  (unsigned long long)ustack_top,
                  (unsigned long long)child_tls_va,
                  (unsigned)cc->thread_stack_owned_count,
                  (unsigned)(spages - cc->thread_stack_owned_count),
                  (unsigned)parent->mem_pages_used,
                  (unsigned)parent->mem_quota_pages);
    return (int64_t)child->pid;

/* ==================================================================
 * 失败路径：回滚
 *
 * 人工必须审查：
 *   - 回滚顺序：
 *       1) 清 PTE（vmm_unmap_page）；
 *       2) 释放物理页（pmm_free_pages）；
 *       3) 递减父进程配额（user_quota_release）；
 *   - 复用父进程 heap 映射的栈页不在 owned 数组中，
 *     不需要（也不能）释放；
 *   - TLS 页若已映射，也要清 PTE + 释放 + 递减配额；
 *   - 最后 task_destroy(child) 释放 task_t / thread / 内核栈。
 * ================================================================== */
rollback_stack:
    /* 先清 PTE、再释放物理页、最后递减配额 */
    for (uint32_t i = 0; i < cc->thread_stack_owned_count; ++i) {
        uint64_t pa = cc->thread_stack_owned_phys[i];
        if (pa == 0) continue;

        /* 找到对应 PTE 并清空 */
        for (uint32_t p = 0; p < spages; ++p) {
            uint64_t va = stack_bottom + (uint64_t)p * PAGE_SIZE;
            uint64_t *pte = vmm_get_pte(pc->pml4, va);
            if (pte && (*pte & PTE_PRESENT) &&
                ((*pte & PTE_ADDR_MASK) == pa)) {
                *pte = 0;
                break;
            }
        }

        struct page *pg = phys_to_page(pa);
        if (pg && !(pg->flags & PG_RESERVED) &&
            !(pg->flags & PG_SLAB))
            pmm_free_pages(pg, 0);

        user_quota_release(parent, 1);
    }
    vmm_flush_tlb();

    ctx_free(child);
    task_destroy(child);

    serial_printf("[USER] thread_spawn FAILED: pid=%llu entry=0x%llx "
                  "(rolled back %u owned pages)\n",
                  (unsigned long long)(child ? child->pid : 0),
                  (unsigned long long)entry,
                  (unsigned)cc->thread_stack_owned_count);
    return OB_ENOMEM;
}
/*===OmniBridgeOs/kernel/arch/x64/user/user.c 结束===*/