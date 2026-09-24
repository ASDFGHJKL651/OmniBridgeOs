/*===OmniBridgeOs/kernel/arch/x64/seccomp.c===*/
#include "seccomp.h"
#include "vmm.h"
#include "kmalloc.h"
#include "serial.h"
#include "user/user.h"
#include "pmm.h"

void seccomp_init(void)
{
    serial_printf("[SECCOMP] init: bitmap covering 0x100..0x17F\n");
}

int seccomp_check(struct task_t *cur, uint64_t nr)
{
    if (!cur) return -1;
    if (cur->seccomp_mode != SECCOMP_MODE_FILTER) return 0;
    if (!cur->seccomp_filter) return 0;

    struct seccomp_filter *f = cur->seccomp_filter;

    /* 只覆盖 0x100..0x17F */
    if (nr < 0x100 || nr > 0x17F) {
        /* 未覆盖范围：按 default_action 处理 */
        switch (f->default_action) {
        case SECCOMP_RET_ALLOW: return 0;
        case SECCOMP_RET_ERRNO: return -(int)f->errno_value;
        case SECCOMP_RET_KILL:
            serial_printf("[SECCOMP] KILL pid=%llu nr=0x%llx\n",
                          (unsigned long long)cur->pid,
                          (unsigned long long)nr);
            task_exit(-9);
            return -9;
        }
        return 0;
    }

    uint32_t idx = (uint32_t)(nr - 0x100);
    int allowed = (f->bitmap[idx >> 3] >> (idx & 7)) & 1;
    if (allowed) return 0;

    switch (f->default_action) {
    case SECCOMP_RET_ALLOW: return 0;
    case SECCOMP_RET_ERRNO:
        serial_printf("[SECCOMP] deny pid=%llu nr=0x%llx -> errno=%u\n",
                      (unsigned long long)cur->pid,
                      (unsigned long long)nr,
                      (unsigned)f->errno_value);
        return -(int)f->errno_value;
    case SECCOMP_RET_KILL:
        serial_printf("[SECCOMP] KILL pid=%llu nr=0x%llx\n",
                      (unsigned long long)cur->pid,
                      (unsigned long long)nr);
        task_exit(-9);
        return -9;
    }
    return 0;
}

int64_t seccomp_syscall(struct task_t *cur, uint64_t mode, uint64_t filt_uaddr)
{
    if (!cur) return -1;

    if (mode == SECCOMP_MODE_DISABLED) {
        if (cur->seccomp_filter) {
            kfree(cur->seccomp_filter);
            cur->seccomp_filter = 0;
        }
        cur->seccomp_mode = SECCOMP_MODE_DISABLED;
        return 0;
    }

    if (mode != SECCOMP_MODE_FILTER) return -22;

    /* 拷贝过滤规则 */
    if (!user_range_ok(filt_uaddr, sizeof(struct seccomp_filter)))
        return -14;

    struct user_ctx *uc = user_get_ctx(cur);
    uint64_t *pml4 = 0;
    if (cur->priv_iso_ready && cur->mem_domain.pml4_self_ptr) {
        pml4 = cur->mem_domain.pml4_self_ptr;
    } else if (uc && uc->pml4) {
        pml4 = uc->pml4;
    } else {
        return -1;
    }

    uint64_t *pte = vmm_get_pte(pml4, filt_uaddr);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -14;
    uint64_t pa = (*pte & PTE_ADDR_MASK) + (filt_uaddr & 0xFFF);
    if ((filt_uaddr & 0xFFF) + sizeof(struct seccomp_filter) > PAGE_SIZE)
        return -14;

    struct seccomp_filter *f =
        (struct seccomp_filter *)kzalloc(sizeof(*f));
    if (!f) return -12;
    const struct seccomp_filter *src =
        (const struct seccomp_filter *)(uintptr_t)(DIRECTMAP_BASE + pa);
    *f = *src;

    if (cur->seccomp_filter) kfree(cur->seccomp_filter);
    cur->seccomp_filter = f;
    cur->seccomp_mode = SECCOMP_MODE_FILTER;

    serial_printf("[SECCOMP] filter installed pid=%llu action=%u errno=%u\n",
                  (unsigned long long)cur->pid,
                  (unsigned)f->default_action,
                  (unsigned)f->errno_value);
    return 0;
}