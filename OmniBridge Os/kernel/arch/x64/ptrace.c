/*===OmniBridgeOs/kernel/arch/x64/ptrace.c===*/
#include "ptrace.h"
#include "vmm.h"
#include "vfs.h"
#include "serial.h"
#include "sched.h"
#include "kmalloc.h"
#include "pmm.h"
#include "user/signal.h"
#include "user/user.h"

void ptrace_init(void)
{
    serial_printf("[PTRACE] init: minimal subset\n");
}

int64_t ptrace_syscall(struct task_t *cur, uint64_t request, uint64_t pid,
                       uint64_t addr, uint64_t data)
{
    if (!cur) return -1;

    switch (request) {
    case PTRACE_TRACEME:
        cur->ptrace_flags |= PTRACE_FLAG_TRACED;
        return 0;

    case PTRACE_ATTACH: {
        struct task_t *target = task_find_by_pid(pid);
        if (!target) return -2;
        if (target->pid <= 99 && cur->pid > 99) return -1;
        if (cur->privilege_level < target->privilege_level) return -1;
        target->ptrace_flags |= PTRACE_FLAG_ATTACHED;
        target->ptrace_tracer_pid = cur->pid;
        return 0;
    }

    case PTRACE_DETACH: {
        struct task_t *target = task_find_by_pid(pid);
        if (!target) return -2;
        if (target->ptrace_tracer_pid != cur->pid) return -1;
        target->ptrace_flags &= ~PTRACE_FLAG_ATTACHED;
        target->ptrace_tracer_pid = 0;
        return 0;
    }

    case PTRACE_PEEKDATA: {
        struct task_t *target = task_find_by_pid(pid);
        if (!target) return -2;
        if (target->ptrace_tracer_pid != cur->pid) return -1;
        struct user_ctx *uc = user_get_ctx(target);
        if (!uc || !uc->pml4) return -1;
        uint64_t *pte = vmm_get_pte(uc->pml4, addr);
        if (!pte || !(*pte & PTE_PRESENT)) return -14;
        uint64_t pa = (*pte & PTE_ADDR_MASK) + (addr & 0xFFF);
        if ((addr & 0xFFF) + 8 > PAGE_SIZE) return -14;
        uint64_t v = *(uint64_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
        return (int64_t)v;
    }

    case PTRACE_POKEDATA: {
        struct task_t *target = task_find_by_pid(pid);
        if (!target) return -2;
        if (target->ptrace_tracer_pid != cur->pid) return -1;
        struct user_ctx *uc = user_get_ctx(target);
        if (!uc || !uc->pml4) return -1;
        uint64_t *pte = vmm_get_pte(uc->pml4, addr);
        if (!pte || !(*pte & PTE_PRESENT)) return -14;
        uint64_t pa = (*pte & PTE_ADDR_MASK) + (addr & 0xFFF);
        if ((addr & 0xFFF) + 8 > PAGE_SIZE) return -14;
        *(uint64_t *)(uintptr_t)(DIRECTMAP_BASE + pa) = data;
        return 0;
    }

    case PTRACE_CONT:
        return 0;

    default:
        return -38;
    }
}

/*
 * 核心转储：写入最小寄存器现场到 /tmp/core.<pid>。
 *
 * 人工必须审查：
 *   - 只写寄存器现场（约 200 字节），不写内存。
 *   - 失败不能影响信号终止流程。
 */
struct core_header {
    uint64_t magic;      /* 0x434F5245444D5000 */
    uint64_t pid;
    int32_t  signum;
    uint32_t _pad;
    uint64_t fault_addr;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

void core_dump(struct task_t *t, int signum, uint64_t fault_addr)
{
    if (!t) return;
    if (t->pid <= 99) return;   /* 不转储保留区 */

    char path[64];
    int i = 0;
    const char *p = "/tmp/core.";
    while (p[i]) { path[i] = p[i]; ++i; }
    uint64_t pid = t->pid;
    char num[24];
    int nd = 0;
    if (pid == 0) num[nd++] = '0';
    while (pid && nd < (int)sizeof(num) - 1) {
        num[nd++] = (char)('0' + (pid % 10));
        pid /= 10;
    }
    for (int k = nd - 1; k >= 0; --k) path[i++] = num[k];
    path[i] = '\0';

    struct vfs_file *f = 0;
    int rc = vfs_open(path, VFS_O_CREAT | VFS_O_RDWR, &f);
    if (rc != 0 || !f) {
        serial_printf("[CORE] cannot open %s rc=%d\n", path, rc);
        return;
    }

    struct core_header ch;
    uint8_t *pc = (uint8_t *)&ch;
    for (unsigned k = 0; k < sizeof(ch); ++k) pc[k] = 0;
    ch.magic = 0x434F5245444D5000ULL;
    ch.pid   = t->pid;
    ch.signum = signum;
    ch.fault_addr = fault_addr;

    struct user_regs *r = signal_handler_slot(t);
    if (r) {
        ch.r15 = r->r15; ch.r14 = r->r14; ch.r13 = r->r13; ch.r12 = r->r12;
        ch.r11 = r->r11; ch.r10 = r->r10; ch.r9 = r->r9; ch.r8 = r->r8;
        ch.rsi = r->rsi; ch.rdi = r->rdi; ch.rbp = r->rbp;
        ch.rdx = r->rdx; ch.rcx = r->rcx; ch.rbx = r->rbx;
        ch.rax = r->rax;
        ch.rip = r->rip; ch.cs = r->cs; ch.rflags = r->rflags;
        ch.rsp = r->rsp; ch.ss = r->ss;
    }

    int64_t nw = vfs_write(f, &ch, sizeof(ch));
    vfs_close(f);

    if (nw == (int64_t)sizeof(ch)) {
        serial_printf("[CORE] wrote %s (%u bytes)\n",
                      path, (unsigned)sizeof(ch));
    } else {
        serial_printf("[CORE] write failed nw=%lld\n", (long long)nw);
    }
}