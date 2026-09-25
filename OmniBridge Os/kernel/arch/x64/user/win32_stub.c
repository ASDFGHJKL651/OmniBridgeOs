/*===OmniBridgeOs/kernel/arch/x64/user/win32_stub.c===*/
/*
 * 用户态 Win32 API stub 页构造 —— 第 20 步扩展版（500 项）。
 *
 * 每个 stub 16 字节：
 *     B8 <imm32>        mov eax, N
 *     49 89 CA          mov r10, rcx
 *     0F 05             syscall
 *     C3                ret
 *     90 90 90 90 90    nop
 *
 * 人工必须审查：
 *   - stub 页 PTE = PTE_PRESENT | PTE_USER（不加 WRITABLE，不加 NX）。
 *   - 500 项 × 16 = 8000 字节，1 MiB 页足够。
 *   - win32_stub_va 的范围检查使用 WIN32_API_FIRST 与
 *     WIN32_API_LAST（避免与 WIN32_API_COUNT 的边界歧义）。
 */
#include "win32_api.h"
#include "user.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"

uint64_t win32_stub_page_bytes(void)
{
    return (uint64_t)WIN32_API_COUNT * WIN32_STUB_STRIDE;
}

uint64_t win32_stub_va(struct task_t *t, uint32_t api_nr)
{
    (void)t;
    if (api_nr < WIN32_API_FIRST || api_nr > WIN32_API_LAST) return 0;
    uint32_t idx = api_nr - WIN32_API_FIRST;
    return WIN32_STUB_BASE + (uint64_t)idx * WIN32_STUB_STRIDE;
}

int win32_stub_init(struct task_t *t, uint64_t stub_base)
{
    if (!t) return -1;
    struct user_ctx *c = user_get_ctx(t);
    if (!c || !c->pml4) return -1;

    uint64_t bytes = win32_stub_page_bytes();
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = 0; p < pages; ++p) {
        uint64_t va = stub_base + p * PAGE_SIZE;

        struct page *pg = pmm_alloc_pages(0);
        if (!pg) return -1;
        uint64_t pa = page_to_phys(pg);

        int rc = vmm_map_page(c->pml4, va, pa,
                              PTE_PRESENT | PTE_USER);
        if (rc != 0) {
            if (rc == -2) { pmm_free_pages(pg, 0); continue; }
            pmm_free_pages(pg, 0);
            return -1;
        }

        uint8_t *dst = (uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
        for (uint64_t k = 0; k < PAGE_SIZE; ++k) dst[k] = 0x90;
    }

    for (uint32_t i = 0; i < WIN32_API_COUNT; ++i) {
        uint32_t nr = WIN32_API_FIRST + i;
        uint64_t va = stub_base + (uint64_t)i * WIN32_STUB_STRIDE;
        uint64_t *pte = vmm_get_pte(c->pml4, va);
        if (!pte || !(*pte & PTE_PRESENT)) return -1;
        uint64_t pa  = *pte & PTE_ADDR_MASK;
        uint64_t off = va & 0xFFFu;
        uint8_t *p = (uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa + off);

        p[0] = 0xB8;
        p[1] = (uint8_t)(nr & 0xFF);
        p[2] = (uint8_t)((nr >> 8) & 0xFF);
        p[3] = (uint8_t)((nr >> 16) & 0xFF);
        p[4] = (uint8_t)((nr >> 24) & 0xFF);
        p[5] = 0x49; p[6] = 0x89; p[7] = 0xCA;
        p[8] = 0x0F; p[9] = 0x05;
        p[10] = 0xC3;
        for (int k = 11; k < 16; ++k) p[k] = 0x90;
    }

    serial_printf("[WIN32] stub page at 0x%llx (%u APIs, %llu bytes)\n",
                  (unsigned long long)stub_base,
                  (unsigned)WIN32_API_COUNT,
                  (unsigned long long)bytes);
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/user/win32_stub.c 结束===*/