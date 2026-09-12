#ifndef OMNIBRIDGE_APIC_H
#define OMNIBRIDGE_APIC_H

#include <stdint.h>

/*
 * 本步骤 APIC 仅提供骨架，默认不启用。
 * 保留接口以便后续步骤切换到 APIC 时无需修改上层调用点。
 */

void lapic_init(void);       /* 初始化 LAPIC（骨架） */
void ioapic_init(void);      /* 初始化 IOAPIC（骨架） */
void apic_enable(void);      /* 使能 APIC（骨架） */

#endif