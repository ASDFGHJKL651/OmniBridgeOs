#ifndef OMNIBRIDGE_LAPIC_H
#define OMNIBRIDGE_LAPIC_H

#include <stdint.h>

/*
 * LAPIC MMIO 寄存器偏移（相对于 APIC MMIO 基址）。
 * 参考：Intel SDM Vol 3A, 10.4.1 Local APIC Register Address Map。
 */
#define LAPIC_ID         0x020u
#define LAPIC_VERSION    0x030u
#define LAPIC_TPR        0x080u
#define LAPIC_APR        0x090u
#define LAPIC_PPR        0x0A0u
#define LAPIC_EOI        0x0B0u
#define LAPIC_RRD        0x0C0u
#define LAPIC_LDR        0x0D0u
#define LAPIC_DFR        0x0E0u
#define LAPIC_SVR        0x0F0u
#define LAPIC_ISR        0x100u
#define LAPIC_TMR        0x180u
#define LAPIC_IRR        0x200u
#define LAPIC_ESR        0x280u
#define LAPIC_ICR_LOW    0x300u
#define LAPIC_ICR_HIGH   0x310u
#define LAPIC_LVT_TIMER  0x320u
#define LAPIC_LVT_THERM  0x330u
#define LAPIC_LVT_PERF   0x340u
#define LAPIC_LVT_LINT0  0x350u
#define LAPIC_LVT_LINT1  0x360u
#define LAPIC_LVT_ERROR  0x370u
#define LAPIC_TIMER_INIT 0x380u
#define LAPIC_TIMER_CUR  0x390u
#define LAPIC_TIMER_DIV  0x3E0u

/* LVT 公共位 */
#define LAPIC_LVT_MASKED    (1u << 16)
#define LAPIC_LVT_PERIODIC  (1u << 17)

/* SVR 位：位 8 = APIC Software Enable */
#define LAPIC_SVR_ENABLE    (1u << 8)

/* 定时器分频选择（写入 LAPIC_TIMER_DIV 的值） */
#define LAPIC_DIV_2    0x00u
#define LAPIC_DIV_4    0x01u
#define LAPIC_DIV_8    0x02u
#define LAPIC_DIV_16   0x03u
#define LAPIC_DIV_32   0x08u
#define LAPIC_DIV_64   0x09u
#define LAPIC_DIV_128  0x0Au

/* ICR 位 */
#define LAPIC_ICR_DELIVERY_FIXED     0x00000u
#define LAPIC_ICR_DELIVERY_INIT      0x00500u
#define LAPIC_ICR_DELIVERY_STARTUP   0x00600u
#define LAPIC_ICR_DEST_PHYSICAL      0x00000u
#define LAPIC_ICR_DEST_LOGICAL       0x00800u
#define LAPIC_ICR_LEVEL_DEASSERT     0x00000u
#define LAPIC_ICR_LEVEL_ASSERT       0x04000u
#define LAPIC_ICR_TRIGGER_EDGE       0x00000u
#define LAPIC_ICR_TRIGGER_LEVEL      0x08000u

/* ICR 发送状态位（bit 12），等待发送完成 */
#define LAPIC_ICR_DELIVERY_PENDING   (1u << 12)

/* 目标简写（ICR[19:18]） */
#define LAPIC_ICR_SHORTHAND_NONE      (0u << 18)
#define LAPIC_ICR_SHORTHAND_SELF      (1u << 18)
#define LAPIC_ICR_SHORTHAND_ALL       (2u << 18)
#define LAPIC_ICR_SHORTHAND_OTHERS    (3u << 18)

/* LAPIC 定时器中断向量（自主选择，不冲突 0..47） */
#define LAPIC_TIMER_VECTOR  0x40u   /* 64 */

/* 默认 MMIO 基址（MSR IA32_APIC_BASE 会给出真实基址） */
#define LAPIC_DEFAULT_BASE  0xFEE00000ULL

/*
 * 初始化 LAPIC：
 *   - 通过 MSR 读取/写入 APIC 基址
 *   - 置位 IA32_APIC_BASE[11] 全局使能 APIC
 *   - 设置 SVR（Software Enable + 伪中断向量 0xFF）
 *   - TPR 清零，屏蔽所有 LVT（定时器稍后由 lapic_timer_init 打开）
 */
void lapic_init(void);

/* 当前 LAPIC ID（取 LAPIC_ID 寄存器高 8 位） */
uint32_t lapic_get_id(void);

/* 发送 EOI：处理完 LAPIC 中断后必须调用（优先于 sched_tick 之前调用） */
void lapic_eoi(void);

/*
 * 初始化 LAPIC 定时器为周期模式，频率 hz（推荐 100）。
 * 向量固定为 LAPIC_TIMER_VECTOR（0x40）。
 * 内部假设总线时钟约 100MHz、分频 16 —— 若目标机器差异较大，
 * 请参考 calibrate 流程（后续步骤补充）。
 */
void lapic_timer_init(uint32_t hz);

/* 停止 LAPIC 定时器（屏蔽 + 清初始计数） */
void lapic_timer_stop(void);

/* 向指定 APIC ID 发送 IPI，向量 vector，固定投递、物理目标、边沿触发 */
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);

/* 向自身发送 IPI（使用目标简写 SELF） */
void lapic_send_ipi_self(uint32_t vector);

/* 向所有其它 CPU 发送 IPI（目标简写 OTHERS，用于 SMP 后续扩展） */
void lapic_send_ipi_others(uint32_t vector);

#endif /* OMNIBRIDGE_LAPIC_H */