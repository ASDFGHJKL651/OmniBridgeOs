#ifndef OMNIBRIDGE_IOAPIC_H
#define OMNIBRIDGE_IOAPIC_H

#include <stdint.h>

/*
 * IOAPIC 默认 MMIO 基址（物理地址），位于低 4GB 恒等映射内。
 * 真实基址应该从 ACPI MADT 表解析；本步先用 QEMU 默认值。
 */
#define IOAPIC_DEFAULT_BASE  0xFEC00000ULL

/* 寄存器索引（IOREGSEL） */
#define IOAPIC_REG_ID       0x00u
#define IOAPIC_REG_VER      0x01u
#define IOAPIC_REG_ARB      0x02u
#define IOAPIC_REG_REDTBL0  0x10u  /* 每个重定向项占 2 个索引 */

/*
 * 初始化 IOAPIC：
 *   - 读取版本，确认重定向项数量
 *   - 屏蔽所有重定向项
 *   - 配置 LAPIC 为物理目标、固定投递
 *
 * 注意：本步默认仍使用 8259 PIC 作为过渡方案（见 pic.c）；
 *       IOAPIC 只做初始化与后续扩展接口。
 */
void ioapic_init(void);

/* 返回 IOAPIC 版本寄存器（包含最大重定向项数量） */
uint32_t ioapic_version(void);

/*
 * 设置一个重定向项。
 *   irq      : 0..23（具体上限由版本决定）
 *   vector   : 目标向量
 *   apic_id  : 目标 LAPIC ID
 *   flags    : 额外位（当前仅支持 0；内部固定为物理目标、固定投递、
 *              高电平有效、边沿触发）
 */
void ioapic_set_redir(uint8_t irq, uint8_t vector, uint8_t apic_id);

void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);

#endif /* OMNIBRIDGE_IOAPIC_H */