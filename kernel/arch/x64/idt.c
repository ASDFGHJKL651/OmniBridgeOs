/*===OmniBridgeOs/kernel/arch/x64/idt.c===*/
#include "idt.h"
#include "pic.h"
#include "printk.h"
#include "serial.h"
#include "vmm.h"
#include "sched.h"
#include "lapic.h"
#include "ipi.h"
#include "task.h"
#include "mem_domain.h"
#include "audit.h"
#include "permission.h"
/* ★ 第 18 步 */
#include "compat_exception.h"
/* ★ 第 18C 步 */
#include "user/user.h"
#include "user/signal.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[256];
static struct idt_ptr   g_idtp;

extern void idt_load(uint64_t idtp_addr);

#define ISR_DECL(n) extern void isr##n(void);
ISR_DECL(0)  ISR_DECL(1)  ISR_DECL(2)  ISR_DECL(3)  ISR_DECL(4)  ISR_DECL(5)
ISR_DECL(6)  ISR_DECL(7)  ISR_DECL(8)  ISR_DECL(9)  ISR_DECL(10) ISR_DECL(11)
ISR_DECL(12) ISR_DECL(13) ISR_DECL(14) ISR_DECL(15) ISR_DECL(16) ISR_DECL(17)
ISR_DECL(18) ISR_DECL(19) ISR_DECL(20) ISR_DECL(21) ISR_DECL(22) ISR_DECL(23)
ISR_DECL(24) ISR_DECL(25) ISR_DECL(26) ISR_DECL(27) ISR_DECL(28) ISR_DECL(29)
ISR_DECL(30) ISR_DECL(31)

#define IRQ_DECL(n) extern void irq##n(void);
IRQ_DECL(0)  IRQ_DECL(1)  IRQ_DECL(2)  IRQ_DECL(3)  IRQ_DECL(4)  IRQ_DECL(5)
IRQ_DECL(6)  IRQ_DECL(7)  IRQ_DECL(8)  IRQ_DECL(9)  IRQ_DECL(10) IRQ_DECL(11)
IRQ_DECL(12) IRQ_DECL(13) IRQ_DECL(14) IRQ_DECL(15)

extern void lapic_timer_isr(void);
extern void ipi_resched_isr(void);

static void set_gate(int vec, void (*fn)(void))
{
    uint64_t addr = (uint64_t)(uintptr_t)fn;
    g_idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    g_idt[vec].selector    = 0x08;
    g_idt[vec].ist         = 0;
    g_idt[vec].type_attr   = 0x8E;
    g_idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vec].zero        = 0;
}

void idt_init(void)
{
    uint8_t *p = (uint8_t *)g_idt;
    for (unsigned i = 0; i < sizeof(g_idt); ++i) p[i] = 0;

    set_gate(0,  isr0);  set_gate(1,  isr1);  set_gate(2,  isr2);  set_gate(3,  isr3);
    set_gate(4,  isr4);  set_gate(5,  isr5);  set_gate(6,  isr6);  set_gate(7,  isr7);
    set_gate(8,  isr8);  set_gate(9,  isr9);  set_gate(10, isr10); set_gate(11, isr11);
    set_gate(12, isr12); set_gate(13, isr13); set_gate(14, isr14); set_gate(15, isr15);
    set_gate(16, isr16); set_gate(17, isr17); set_gate(18, isr18); set_gate(19, isr19);
    set_gate(20, isr20); set_gate(21, isr21); set_gate(22, isr22); set_gate(23, isr23);
    set_gate(24, isr24); set_gate(25, isr25); set_gate(26, isr26); set_gate(27, isr27);
    set_gate(28, isr28); set_gate(29, isr29); set_gate(30, isr30); set_gate(31, isr31);

    set_gate(32, irq0);  set_gate(33, irq1);  set_gate(34, irq2);  set_gate(35, irq3);
    set_gate(36, irq4);  set_gate(37, irq5);  set_gate(38, irq6);  set_gate(39, irq7);
    set_gate(40, irq8);  set_gate(41, irq9);  set_gate(42, irq10); set_gate(43, irq11);
    set_gate(44, irq12); set_gate(45, irq13); set_gate(46, irq14); set_gate(47, irq15);

    set_gate(64, lapic_timer_isr);
    set_gate(80, ipi_resched_isr);

    g_idtp.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idtp.base  = (uint64_t)(uintptr_t)&g_idt[0];
    idt_load((uint64_t)(uintptr_t)&g_idtp);

    serial_printf("[IDT] loaded: 0-31 exceptions, 32-47 IRQs, 64 LAPIC, 80 IPI\n");
}

#ifdef OB_QEMU_EXIT
extern void qemu_exit(uint32_t code);
#endif

void page_fault_handler(struct regs *r)
{
    uint64_t cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));

    uint64_t err = r->error;

    int present  = (int)(err & 1);
    int is_write = (int)((err >> 1) & 1);
    int is_user  = (int)((err >> 2) & 1);
    int is_rsvd  = (int)((err >> 3) & 1);
    int is_fetch = (int)((err >> 4) & 1);

    /* ★ 第 18 步：兼容层异常转换优先 */
    {
        struct task_t *cur = task_from_thread(sched_current());
        if (compat_exception_handle(r, cur)) {
            task_exit(-1);
        }
    }

    serial_printf("\n[PF] Page Fault: CR2=0x%llx err=0x%llx\n",
                  (unsigned long long)cr2,
                  (unsigned long long)err);
    serial_printf("[PF]   P=%d (%s) W=%d (%s) U=%d (%s) RSVD=%d I=%d (%s)\n",
                  present, present ? "protection" : "not-present",
                  is_write, is_write ? "write" : "read",
                  is_user, is_user ? "user" : "kernel",
                  is_rsvd,
                  is_fetch, is_fetch ? "instruction fetch" : "data");
    serial_printf("[PF]   RIP=0x%llx CS=0x%llx RFLAGS=0x%llx\n",
                  (unsigned long long)r->rip,
                  (unsigned long long)r->cs,
                  (unsigned long long)r->rflags);

    /* ★★★ 第 18C 步修复：用户态 #PF → 信号 → user_resume ★★★
     *
     * 人工必须审查：
     *   - (r->cs & 3) == 3 表示异常来自 CPL=3 用户态；此时不能 panic。
     *   - signal_from_exception 会从 r 提取真实的用户态 RSP/RFLAGS/SS。
     *   - 若 handler 已安装（rc==1），调用 user_resume iretq 回用户态。
     *   - 若 rc==0（SIG_IGN 或未注册），走默认动作（task_exit）。
     */
    {
        struct task_t *cur = task_from_thread(sched_current());
        if (cur && (r->cs & 3) == 3) {
            serial_printf("[PF] user-mode #PF -> SIGSEGV pid=%llu\n",
                        (unsigned long long)cur->pid);
            int rc = signal_from_exception(cur, r);
            if (rc == 1) {
                user_resume_ctx(cur, signal_handler_slot(cur));  /* ★ 必须是 handler_slot */
            }
            task_exit(-11);
        }
    }

    {
        struct thread *th = sched_current();
        struct task_t *cur = th ? task_from_thread(th) : 0;

        if (cur &&
            (cur->privilege_level == 0 || cur->privilege_level == 1) &&
            cur->priv_iso_ready) {

            if (cr2 < KERNEL_SPACE_START) {
                if (!mem_domain_contains(cur, cr2)) {
                    serial_printf("[PF] domain violation pid=%llu "
                                  "cr2=0x%llx err=0x%llx\n",
                                  (unsigned long long)cur->pid,
                                  (unsigned long long)cr2,
                                  (unsigned long long)err);
                    audit_critical_access(cur->pid,
                                          "(pf-outside-domain)",
                                          is_write ? OB_ACCESS_WRITE
                                                   : OB_ACCESS_READ);
                    task_exit(-1);
                }

                serial_printf("[PF] unmapped-in-domain pid=%llu "
                              "cr2=0x%llx err=0x%llx\n",
                              (unsigned long long)cur->pid,
                              (unsigned long long)cr2,
                              (unsigned long long)err);
                task_exit(-1);
            }
        }
    }

    int in_kernel_space = (cr2 >= KERNEL_SPACE_START);

    if (is_user && in_kernel_space) {
        serial_printf("[PF] U/S=1 且 CR2 位于内核空间：用户态非法访问内核地址\n");
        serial_printf("[PF] （未来接入进程子系统后，此处终止该进程）\n");
        serial_printf("[PF] System halted (simulated process termination).\n");
#ifdef OB_QEMU_EXIT
        qemu_exit(1);
#endif
        for (;;) { __asm__ __volatile__("hlt"); }
    }

    serial_printf("[PF] Kernel-mode page fault, PANIC.\n");
    serial_printf("[PF] System halted.\n");
#ifdef OB_QEMU_EXIT
    qemu_exit(1);
#endif
    for (;;) { __asm__ __volatile__("hlt"); }
}

void exception_handler(struct regs *r)
{
    /* ★ 第 18 步：兼容层异常转换优先（含 #PF，但 #PF 已在上面处理） */
    {
        struct task_t *cur = task_from_thread(sched_current());
        if (compat_exception_handle(r, cur)) {
            task_exit(-1);
        }
    }

    /* ★★★ 第 18C 步修复：用户态异常 → 信号 → user_resume ★★★ */
    {
        struct task_t *cur = task_from_thread(sched_current());
        if (cur && (r->cs & 3) == 3 && r->vector != 14) {
            serial_printf("[EXC] user-mode vec=%llu -> signal pid=%llu\n",
                          (unsigned long long)r->vector,
                          (unsigned long long)cur->pid);
            int rc = signal_from_exception(cur, r);
            if (rc == 1) {
                user_resume_ctx(cur, signal_handler_slot(cur));  /* ★ 修改 */
                /* 不返回 */
            }
            task_exit(-(int)r->vector);
        }
    }

    if (r->vector == 14) {
        page_fault_handler(r);
        return;
    }

    const char *name = "Unknown";
    switch (r->vector) {
    case 0:  name = "Divide Error";               break;
    case 1:  name = "Debug";                      break;
    case 2:  name = "NMI";                        break;
    case 3:  name = "Breakpoint";                 break;
    case 4:  name = "Overflow";                   break;
    case 5:  name = "BOUND Range";                break;
    case 6:  name = "Invalid Opcode";             break;
    case 7:  name = "Device Not Available";       break;
    case 8:  name = "Double Fault";               break;
    case 9:  name = "Coprocessor Segment Overrun";break;
    case 10: name = "Invalid TSS";                break;
    case 11: name = "Segment Not Present";        break;
    case 12: name = "Stack-Segment Fault";        break;
    case 13: name = "General Protection Fault";   break;
    case 16: name = "x87 FP Error";               break;
    case 17: name = "Alignment Check";            break;
    case 18: name = "Machine Check";              break;
    case 19: name = "SIMD FP Error";              break;
    case 20: name = "Virtualization";             break;
    case 21: name = "Control Protection";         break;
    default: break;
    }

    serial_printf("\n[PANIC] Exception %llu: %s\n",
                  (unsigned long long)r->vector, name);
    serial_printf("  RIP=%p CS=%llx RFLAGS=%llx ERR=%llx\n",
                  (void *)r->rip, (unsigned long long)r->cs,
                  (unsigned long long)r->rflags,
                  (unsigned long long)r->error);

    serial_printf("System halted.\n");

#ifdef OB_QEMU_EXIT
    qemu_exit(1);
#endif
    for (;;) { __asm__ __volatile__("hlt"); }
}

static volatile uint64_t g_irq_ticks = 0;

void irq_handler(struct regs *r)
{
    if (r->vector == 64) {
        lapic_eoi();
        sched_tick();
        return;
    }

    if (r->vector == 80) {
        lapic_eoi();
        ipi_handler(IPI_VECTOR_RESCHED);
        return;
    }

    uint64_t irq = r->vector - 32;

    if (irq == 0) {
        g_irq_ticks++;
        if ((g_irq_ticks % 100) == 0) {
            serial_printf("[PIT] tick=%llu\n",
                          (unsigned long long)g_irq_ticks);
        }
    } else {
        serial_printf("[IRQ] vector=%llu\n",
                      (unsigned long long)r->vector);
    }

    if (irq < 16) {
        pic_send_eoi((uint8_t)irq);
    }
}
/*===OmniBridgeOs/kernel/arch/x64/idt.c 结束===*/