#ifndef OMNIBRIDGE_IPI_H
#define OMNIBRIDGE_IPI_H

#include <stdint.h>

/*
 * IPI（Inter-Processor Interrupt）向量分配：
 *   0x50 = 抢占重调度 IPI（本步仅单核，用于框架自测）
 *   0x51 = TLB shootdown（预留，未实现）
 *
 * 注意：0x40 已被 LAPIC 定时器占用，不能重复使用。
 */
#define IPI_VECTOR_RESCHED   0x50u
#define IPI_VECTOR_TLB_FLUSH 0x51u

void ipi_init(void);

/* 向指定 APIC ID 发送重调度 IPI */
void ipi_send_resched(uint32_t apic_id);

/* 向自身发送重调度 IPI（单核测试用） */
void ipi_send_resched_self(void);

/* IPI 中断处理入口（由 irq_handler 分派到该函数） */
void ipi_handler(uint32_t vector);

#endif /* OMNIBRIDGE_IPI_H */