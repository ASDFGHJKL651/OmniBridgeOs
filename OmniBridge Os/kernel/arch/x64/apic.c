#include "apic.h"
#include "serial.h"

/*
 * apic.c 保留为兼容性占位：真正实现位于 lapic.c / ioapic.c。
 * 本文件在 Makefile 中仍然存在以避免 vpath 重构；
 * 若将来不需要，可整体删除。
 *
 * 注意：本步开始，默认启动路径已经从 8259 PIC 切换到 LAPIC 定时器。
 *       pic_init/pit_init 仍被 main.c 调用，用于保持 PIC 处于可编程状态，
 *       但不 unmask 任何 IRQ。
 */

void lapic_init_legacy(void) { (void)0; }
void ioapic_init_legacy(void) { (void)0; }
void apic_enable(void) { (void)0; }