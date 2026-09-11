#include "idt.h"
#include "printk.h"
#include "serial.h"

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

/* 由 entry.S 定义 */
#define ISR_DECL(n) extern void isr##n(void);
ISR_DECL(0)  ISR_DECL(1)  ISR_DECL(2)  ISR_DECL(3)  ISR_DECL(4)  ISR_DECL(5)
ISR_DECL(6)  ISR_DECL(7)  ISR_DECL(8)  ISR_DECL(9)  ISR_DECL(10) ISR_DECL(11)
ISR_DECL(12) ISR_DECL(13) ISR_DECL(14) ISR_DECL(15) ISR_DECL(16) ISR_DECL(17)
ISR_DECL(18) ISR_DECL(19) ISR_DECL(20) ISR_DECL(21) ISR_DECL(22) ISR_DECL(23)
ISR_DECL(24) ISR_DECL(25) ISR_DECL(26) ISR_DECL(27) ISR_DECL(28) ISR_DECL(29)
ISR_DECL(30) ISR_DECL(31)

static void set_gate(int vec, void (*fn)(void))
{
    uint64_t addr = (uint64_t)fn;
    g_idt[vec].offset_low  = (uint16_t)(addr & 0xFFFF);
    g_idt[vec].selector    = 0x08;      /* kernel CS */
    g_idt[vec].ist         = 0;
    g_idt[vec].type_attr   = 0x8E;      /* present, DPL0, 64-bit interrupt gate */
    g_idt[vec].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    g_idt[vec].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vec].zero        = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 256; ++i) {
        ((uint8_t *)&g_idt[i])[0] = 0;
        for (unsigned j = 0; j < sizeof(g_idt[i]); ++j)
            ((uint8_t *)&g_idt[i])[j] = 0;
    }

    set_gate(0,  isr0);  set_gate(1,  isr1);  set_gate(2,  isr2);  set_gate(3,  isr3);
    set_gate(4,  isr4);  set_gate(5,  isr5);  set_gate(6,  isr6);  set_gate(7,  isr7);
    set_gate(8,  isr8);  set_gate(9,  isr9);  set_gate(10, isr10); set_gate(11, isr11);
    set_gate(12, isr12); set_gate(13, isr13); set_gate(14, isr14); set_gate(15, isr15);
    set_gate(16, isr16); set_gate(17, isr17); set_gate(18, isr18); set_gate(19, isr19);
    set_gate(20, isr20); set_gate(21, isr21); set_gate(22, isr22); set_gate(23, isr23);
    set_gate(24, isr24); set_gate(25, isr25); set_gate(26, isr26); set_gate(27, isr27);
    set_gate(28, isr28); set_gate(29, isr29); set_gate(30, isr30); set_gate(31, isr31);

    g_idtp.limit = sizeof(g_idt) - 1;
    g_idtp.base  = (uint64_t)&g_idt[0];
    idt_load((uint64_t)&g_idtp);
    printk("[IDT] loaded\n");
}

#ifdef OB_QEMU_EXIT
extern void qemu_exit(uint32_t code);
#endif

void exception_handler(struct regs *r)
{
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
    case 10: name = "Invalid TSS";                break;
    case 11: name = "Segment Not Present";        break;
    case 12: name = "Stack-Segment Fault";        break;
    case 13: name = "General Protection Fault";   break;
    case 14: name = "Page Fault";                 break;
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

    if (r->vector == 14) {
        uint64_t cr2;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
        serial_printf("  CR2=%p\n", (void *)cr2);
    }
    serial_printf("System halted.\n");

#ifdef OB_QEMU_EXIT
    qemu_exit(1);
#endif
    for (;;) { __asm__ __volatile__("hlt"); }
}