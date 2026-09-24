/*===OmniBridgeOs/kernel/arch/x64/user/signal.c===*/
#include "signal.h"
#include "user.h"
#include "idt.h"
#include "serial.h"
#include "audit.h"
#include "sched.h"
#include "task.h"
#include "vmm.h"

#define SIG_TASK_MAX 256

struct sig_state {
    struct ob_sigaction actions[OB_NSIG];
    uint64_t pending;
    uint64_t blocked;
    uint64_t in_handler;
    struct user_regs resume;        /* 原始上下文，用于 sigreturn 恢复 */
    struct user_regs handler_regs;  /* ★ 新增：进入 handler 的上下文 */
    uint64_t fault_rip;
    uint32_t fault_insn_len;
    uint32_t _pad;
    uint8_t valid;
};

static struct sig_state g_sig[SIG_TASK_MAX];
static int g_sig_inited = 0;

void signal_init(void)
{
    if (g_sig_inited) return;
    for (int i = 0; i < SIG_TASK_MAX; ++i) {
        g_sig[i].valid = 0;
        g_sig[i].pending = 0;
        g_sig[i].blocked = 0;
        g_sig[i].in_handler = 0;
        g_sig[i].fault_rip = 0;
        g_sig[i].fault_insn_len = 0;
        for (int k = 0; k < OB_NSIG; ++k) {
            g_sig[i].actions[k].handler = 0;
            g_sig[i].actions[k].sa_mask = 0;
            g_sig[i].actions[k].sa_flags = 0;
        }
    }
    g_sig_inited = 1;
    serial_printf("[SIGNAL] init: NSIG=%d\n", OB_NSIG);
}

void signal_init_task(struct task_t *t)
{
    if (!t) return;
    uint64_t k = t->pid % SIG_TASK_MAX;
    struct sig_state *s = &g_sig[k];
    for (int i = 0; i < OB_NSIG; ++i) {
        s->actions[i].handler = 0;
        s->actions[i].sa_mask = 0;
        s->actions[i].sa_flags = 0;
    }
    s->pending = 0;
    s->blocked = 0;
    s->in_handler = 0;
    s->fault_rip = 0;
    s->fault_insn_len = 0;
    s->valid = 1;
}

void signal_free_task(struct task_t *t)
{
    if (!t) return;
    g_sig[t->pid % SIG_TASK_MAX].valid = 0;
}

static struct sig_state *sig_of(struct task_t *t)
{
    if (!t) return 0;
    struct sig_state *s = &g_sig[t->pid % SIG_TASK_MAX];
    return s->valid ? s : 0;
}

struct user_regs *signal_resume_slot(struct task_t *t)
{
    struct sig_state *s = sig_of(t);
    return s ? &s->resume : 0;
}

/* ★ 新增：返回进入 handler 的上下文 */
struct user_regs *signal_handler_slot(struct task_t *t)
{
    struct sig_state *s = sig_of(t);
    return s ? &s->handler_regs : 0;
}

/*
 * ============================================================
 * ★ 第 18C 步新增：故障指令长度译码
 * ============================================================
 */

static int read_user_byte(struct user_ctx *c, uint64_t va, uint8_t *out)
{
    if (!c || !c->pml4) return -1;
    uint64_t *pte = vmm_get_pte(c->pml4, va);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -1;
    uint64_t pa = *pte & PTE_ADDR_MASK;
    uint64_t off = va & 0xFFF;
    *out = *(volatile uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa + off);
    return 0;
}

static uint32_t decode_div_insn_len(struct user_ctx *c, uint64_t rip)
{
    uint8_t buf[8];
    for (int i = 0; i < 8; ++i) {
        if (read_user_byte(c, rip + i, &buf[i]) != 0) return 0;
    }

    int p = 0;
    while (p < 4) {
        uint8_t b = buf[p];
        if (b == 0x66 || b == 0x67 || (b >= 0x40 && b <= 0x4F)) {
            p++;
            continue;
        }
        break;
    }

    if (buf[p] != 0xF6 && buf[p] != 0xF7) return 0;

    uint8_t modrm = buf[p + 1];
    int reg = (modrm >> 3) & 7;
    if (reg != 6 && reg != 7) return 0;

    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;

    int tail = 0;
    if (mod == 0) {
        if (rm == 5) {
            tail = 4;
        } else if (rm == 4) {
            uint8_t sib = buf[p + 2];
            tail = 1;
            if ((sib & 7) == 5) tail += 4;
        }
    } else if (mod == 1) {
        tail = 1;
        if (rm == 4) tail += 1;
    } else if (mod == 2) {
        tail = 4;
        if (rm == 4) tail += 1;
    } else {
        tail = 0;
    }

    return (uint32_t)(p + 2 + tail);
}

static uint32_t compute_fault_insn_len(struct task_t *t, uint64_t vector,
                                       uint64_t rip)
{
    struct user_ctx *c = user_get_ctx(t);
    if (!c) return 0;

    if (vector == 0) {
        return decode_div_insn_len(c, rip);
    }
    return 0;
}

/*
 * ★ 第 18C 步修复：MS ABI 参数寄存器 + 栈对齐。
 */
static int setup_user_handler(struct task_t *t, struct sig_state *s,
                              int signum, struct user_regs *cur)
{
    ob_sighandler_t h = s->actions[signum].handler;

    if (h == (ob_sighandler_t)OB_SIG_IGN) {
        serial_printf("[SIGNAL] sig=%d ignored pid=%llu\n",
                      signum, (unsigned long long)t->pid);
        return 0;
    }
    if (h == (ob_sighandler_t)OB_SIG_DFL) {
        serial_printf("[SIGNAL] default action sig=%d pid=%llu\n",
                      signum, (unsigned long long)t->pid);
        /* ★ 第 18D 步任务 9：核心转储 */
        if (signum == OB_SIGSEGV || signum == OB_SIGFPE ||
            signum == OB_SIGILL || signum == OB_SIGBUS) {
            extern void core_dump(struct task_t *t, int signum,
                                   uint64_t fault_addr);
            core_dump(t, signum, s->fault_rip);
        }
        task_exit(-signum);
        return 0;
    }

    struct user_ctx *c = user_get_ctx(t);
    if (!c) { task_exit(-signum); return 0; }

    /* 1) 保存原始上下文到 s->resume */
    s->resume = *cur;

    /* 2) 在用户栈上放置 sigreturn trampoline 地址 */
    uint64_t user_sp = cur->rsp;
    user_sp -= 48;
    user_sp &= ~0xFULL;
    user_sp -= 8;
    uint64_t tramp = USER_SIGTRAMP_ADDR;
    if (user_stack_write(c, user_sp, &tramp, 8) != 0) {
        serial_printf("[SIGNAL] WARN: cannot write trampoline on user stack\n");
        task_exit(-signum);
        return 0;
    }

    /* 3) 构造进入 handler 的上下文到 s->handler_regs */
    s->handler_regs = *cur;
    s->handler_regs.rip = (uint64_t)(uintptr_t)h;
    s->handler_regs.rsp = user_sp;
    s->handler_regs.rcx = (uint64_t)signum;      /* MS ABI 第一参数 */
    s->handler_regs.rdi = (uint64_t)signum;      /* SysV 兼容 */
    s->handler_regs.rax = 0;

    s->in_handler |= (1ULL << signum);

    serial_printf("[SIGNAL] deliver sig=%d to pid=%llu handler=0x%llx "
                  "rip=0x%llx rsp=0x%llx\n",
                  signum, (unsigned long long)t->pid,
                  (unsigned long long)(uintptr_t)h,
                  (unsigned long long)s->handler_regs.rip,
                  (unsigned long long)s->handler_regs.rsp);
    return 1;
}

/*
 * 从异常现场递送信号。
 */
int signal_from_exception(struct task_t *t, const struct regs *r)
{
    if (!t || !r) return 0;

    if ((r->cs & 3) != 3) return 0;

    struct sig_state *s = sig_of(t);
    if (!s) return 0;

    int signum;
    switch (r->vector) {
    case 0:  signum = OB_SIGFPE;  break;
    case 6:  signum = OB_SIGILL;  break;
    case 13: signum = OB_SIGSEGV; break;
    case 14: signum = OB_SIGSEGV; break;
    case 17: signum = OB_SIGBUS;  break;
    default: signum = OB_SIGTERM; break;
    }

    serial_printf("[SIGNAL] exception vec=%llu -> sig=%d pid=%llu "
                  "rip=0x%llx\n",
                  (unsigned long long)r->vector, signum,
                  (unsigned long long)t->pid,
                  (unsigned long long)r->rip);
    audit_event(AUDIT_EV_COMPAT_EXCEPTION, AUDIT_LVL_CRITICAL,
                t->pid, (uint64_t)r->vector, r->rip, 0, "(user-exception)");

    /* ★ 保存故障指令信息，供 sigreturn 修正 RIP */
    s->fault_rip = r->rip;
    s->fault_insn_len = compute_fault_insn_len(t, r->vector, r->rip);

    if (s->fault_insn_len > 0) {
        serial_printf("[SIGNAL] fault_rip=0x%llx insn_len=%u "
                      "(sigreturn will skip)\n",
                      (unsigned long long)s->fault_rip,
                      (unsigned)s->fault_insn_len);
    }

    /* 构造"当前用户态上下文" */
    const uint64_t *raw = (const uint64_t *)r;
    uint64_t user_rsp = raw[20];
    uint64_t user_ss  = raw[21];

    struct user_regs cur;
    uint8_t *p = (uint8_t *)&cur;
    for (unsigned i = 0; i < sizeof(cur); ++i) p[i] = 0;

    cur.r15 = r->r15; cur.r14 = r->r14; cur.r13 = r->r13; cur.r12 = r->r12;
    cur.r11 = r->r11; cur.r10 = r->r10; cur.r9  = r->r9;  cur.r8  = r->r8;
    cur.rsi = r->rsi; cur.rdi = r->rdi; cur.rbp = r->rbp;
    cur.rdx = r->rdx; cur.rcx = r->rcx; cur.rbx = r->rbx;
    cur.rax = r->rax;
    cur.rip    = r->rip;
    cur.cs     = r->cs;
    cur.rflags = r->rflags;
    cur.rsp    = user_rsp;
    cur.ss     = user_ss;

    return setup_user_handler(t, s, signum, &cur);
}

int signal_deliver_if_pending(struct task_t *t)
{
    struct sig_state *s = sig_of(t);
    if (!s) return 0;

    uint64_t mask = s->pending & ~s->blocked;
    if (!mask) return 0;

    for (int sig = 1; sig < OB_NSIG; ++sig) {
        if (!(mask & (1ULL << sig))) continue;
        s->pending &= ~(1ULL << sig);

        if (sig == OB_SIGKILL) {
            serial_printf("[SIGNAL] pid=%llu killed by SIGKILL\n",
                          (unsigned long long)t->pid);
            task_exit(-sig);
            return 1;
        }

        struct user_regs cur;
        uint8_t *p = (uint8_t *)&cur;
        for (unsigned i = 0; i < sizeof(cur); ++i) p[i] = 0;
        struct user_ctx *c = user_get_ctx(t);
        cur.rip = 0;
        cur.cs  = 0x23;
        cur.ss  = 0x1B;
        cur.rflags = 0x202;
        cur.rsp = c ? (c->stack_top - 0x100) : 0;

        return setup_user_handler(t, s, sig, &cur);
    }
    return 0;
}

/*
 * ★ 第 18C 步新增：sigreturn 前修正 resume 上下文。
 */
int signal_fixup_resume(struct task_t *t)
{
    struct sig_state *s = sig_of(t);
    if (!s) return 0;

    if (s->fault_insn_len == 0) return 0;
    if (s->fault_rip == 0)      return 0;

    if (s->resume.rip != s->fault_rip) {
        serial_printf("[SIGNAL] fixup: handler modified rip "
                      "0x%llx -> 0x%llx (not advancing)\n",
                      (unsigned long long)s->fault_rip,
                      (unsigned long long)s->resume.rip);
        return 0;
    }

    uint64_t old_rip = s->resume.rip;
    s->resume.rip = old_rip + s->fault_insn_len;

    serial_printf("[SIGNAL] fixup: advance rip 0x%llx -> 0x%llx "
                  "(skip fault insn, len=%u)\n",
                  (unsigned long long)old_rip,
                  (unsigned long long)s->resume.rip,
                  (unsigned)s->fault_insn_len);

    s->fault_rip = 0;
    s->fault_insn_len = 0;

    return 1;
}

int64_t sys_signal(int signum, ob_sighandler_t handler)
{
    if (signum <= 0 || signum >= OB_NSIG) return OB_EINVAL;
    uint64_t h = (uint64_t)(uintptr_t)handler;
    if (h != 0 && h != 1) {
        if (!user_range_ok(h, 1)) return OB_EFAULT;
    }
    struct task_t *t = task_from_thread(sched_current());
    struct sig_state *s = sig_of(t);
    if (!s) return OB_EINVAL;
    ob_sighandler_t old = s->actions[signum].handler;
    s->actions[signum].handler = handler;
    return (int64_t)(uintptr_t)old;
}

int64_t sys_sigaction(int signum, const struct ob_sigaction *act,
                      struct ob_sigaction *old)
{
    if (signum <= 0 || signum >= OB_NSIG) return OB_EINVAL;
    struct task_t *t = task_from_thread(sched_current());
    struct sig_state *s = sig_of(t);
    if (!s) return OB_EINVAL;

    if (old) {
        if (copy_to_user((uint64_t)(uintptr_t)old,
                         &s->actions[signum], sizeof(*old)) != 0)
            return OB_EFAULT;
    }
    if (act) {
        struct ob_sigaction local;
        if (copy_from_user(&local, (uint64_t)(uintptr_t)act,
                           sizeof(local)) != 0) return OB_EFAULT;
        uint64_t h = (uint64_t)(uintptr_t)local.handler;
        if (h != 0 && h != 1 && !user_range_ok(h, 1)) return OB_EFAULT;
        s->actions[signum] = local;
    }
    return 0;
}

int64_t sys_sigprocmask(int how, const uint64_t *set, uint64_t *oldset)
{
    struct task_t *t = task_from_thread(sched_current());
    struct sig_state *s = sig_of(t);
    if (!s) return OB_EINVAL;

    if (oldset) {
        if (copy_to_user((uint64_t)(uintptr_t)oldset,
                         &s->blocked, sizeof(s->blocked)) != 0)
            return OB_EFAULT;
    }
    if (set) {
        uint64_t v;
        if (copy_from_user(&v, (uint64_t)(uintptr_t)set, sizeof(v)) != 0)
            return OB_EFAULT;
        if (how == 0) s->blocked |= v;
        else if (how == 1) s->blocked &= ~v;
        else if (how == 2) s->blocked = v;
        else return OB_EINVAL;
    }
    return 0;
}

int64_t sys_kill(uint64_t pid, int signum)
{
    struct task_t *t = task_from_thread(sched_current());
    if (!t) return OB_EPERM;
    if (signum <= 0 || signum >= OB_NSIG) return OB_EINVAL;

    struct task_t *target = task_find_by_pid(pid);
    if (!target) return OB_ENOENT;

    int rc = check_pid_access(t, pid);
    if (rc != 0) return rc;

    struct sig_state *s = sig_of(target);
    if (!s) return OB_ENOENT;

    s->pending |= (1ULL << signum);

    if (target == t) {
        signal_deliver_if_pending(t);
    }
    return 0;
}

int64_t sys_raise(int signum)
{
    struct task_t *t = task_from_thread(sched_current());
    if (!t) return OB_EPERM;
    return sys_kill(t->pid, signum);
}
/*===OmniBridgeOs/kernel/arch/x64/user/signal.c 结束===*/