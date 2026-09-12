#include "apic.h"
#include "serial.h"

/*
 * 第 4 步仅提供骨架：真正的 LAPIC/IOAPIC 编程在后续步骤（SMP）中实现。
 * 现在的默认启动路径仍走 8259 PIC（见 pic.c），因此这里不触碰硬件。
 */

void lapic_init(void)
{
    serial_printf("[APIC] lapic_init: skeleton (not used yet)\n");
}

void ioapic_init(void)
{
    serial_printf("[APIC] ioapic_init: skeleton (not used yet)\n");
}

void apic_enable(void)
{
    serial_printf("[APIC] apic_enable: skeleton (not used yet)\n");
}