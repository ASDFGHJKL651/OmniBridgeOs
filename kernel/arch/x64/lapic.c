/*===OmniBridgeOs/kernel/arch/x64/lapic.c===*/
#include "lapic.h"
#include "serial.h"
#include "vmm.h"                    /* ★ 第 18C 步：DIRECTMAP_BASE */

/*
 * 人工必须审查：
 *   - 所有对 LAPIC 寄存器的访问都必须是 32 位 MMIO。
 *   - ★ 第 18C 步：LAPIC 基址（通常 0xFEE00000）通过 DirectMap 访问。
 *     原因：vmm_create_address_space 不再复制 PML4[0]（内核低恒等
 *     映射），syscall 返回内核后访问物理地址 0xFEE00000 必须走
 *     DirectMap 虚拟地址 0xFFFF800000000000 + 0xFEE00000。
 *   - DirectMap 由 PML4[256] 提供，在所有 PML4 中被复制。
 *   - 写 ICR 后必须等待 Delivery Status = 0。
 */

#define MSR_IA32_APIC_BASE  0x1Bu

static inline uint64_t ob_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr"
                         : "=a"(lo), "=d"(hi)
                         : "c"(msr)
                         : "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void ob_wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ __volatile__("wrmsr"
                         :
                         : "c"(msr),
                           "a"((uint32_t)(v & 0xFFFFFFFFu)),
                           "d"((uint32_t)(v >> 32))
                         : "memory");
}

static volatile uint8_t *g_lapic_base = 0;

static inline uint32_t lapic_read(uint32_t reg)
{
    volatile uint32_t *p = (volatile uint32_t *)(g_lapic_base + reg);
    return *p;
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    volatile uint32_t *p = (volatile uint32_t *)(g_lapic_base + reg);
    *p = val;
    (void)*p;
}

static uint64_t lapic_base_msr_value(void)
{
    return ob_rdmsr(MSR_IA32_APIC_BASE);
}

void lapic_init(void)
{
    /* 1) 读取 IA32_APIC_BASE */
    uint64_t apic_msr = lapic_base_msr_value();
    uint64_t base = apic_msr & 0xFFFFF000ULL;
    if (base == 0) base = LAPIC_DEFAULT_BASE;

    /*
     * ★ 第 18C 步：通过 DirectMap 访问 LAPIC。
     *   不再依赖 PML4[0] 的内核低恒等映射。
     */
    g_lapic_base = (volatile uint8_t *)(DIRECTMAP_BASE + base);

    /* 2) 置位 APIC Global Enable，关闭 x2APIC */
    apic_msr |= (1ULL << 11);
    apic_msr &= ~(1ULL << 10);
    ob_wrmsr(MSR_IA32_APIC_BASE, apic_msr);

    /* 3) SVR */
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFFu);

    /* 4) TPR = 0 */
    lapic_write(LAPIC_TPR, 0);

    /* 5) DFR / LDR */
    lapic_write(LAPIC_DFR, 0xFFFFFFFFu);
    lapic_write(LAPIC_LDR, 0x01000000u);

    /* 6) 屏蔽所有 LVT */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERM, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,  LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);

    /* 7) 清 ESR */
    lapic_write(LAPIC_ESR, 0);
    (void)lapic_read(LAPIC_ESR);
    lapic_write(LAPIC_ESR, 0);

    serial_printf("[LAPIC] base=0x%llx vaddr=0x%llx apic_id=%u version=0x%x\n",
                  (unsigned long long)base,
                  (unsigned long long)(uintptr_t)g_lapic_base,
                  (unsigned)(lapic_read(LAPIC_ID) >> 24),
                  (unsigned)(lapic_read(LAPIC_VERSION) & 0xFFu));
}

uint32_t lapic_get_id(void)
{
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void)
{
    if (g_lapic_base) {
        lapic_write(LAPIC_EOI, 0);
    }
}

void lapic_timer_init(uint32_t hz)
{
    if (hz == 0) hz = 100;

    lapic_write(LAPIC_TIMER_DIV, LAPIC_DIV_16);
    lapic_write(LAPIC_LVT_TIMER,
                (uint32_t)LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC);

    uint32_t count = 6250000u / hz;
    if (count < 16) count = 16;
    lapic_write(LAPIC_TIMER_INIT, count);

    serial_printf("[LAPIC] timer at %u Hz (div=16, count=%u, vec=0x%x)\n",
                  (unsigned)hz, (unsigned)count,
                  (unsigned)LAPIC_TIMER_VECTOR);
}

void lapic_timer_stop(void)
{
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0);
}

void lapic_send_ipi(uint32_t apic_id, uint32_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, (apic_id & 0xFFu) << 24);
    lapic_write(LAPIC_ICR_LOW,
                (vector & 0xFFu) |
                LAPIC_ICR_DELIVERY_FIXED |
                LAPIC_ICR_DEST_PHYSICAL |
                LAPIC_ICR_LEVEL_ASSERT |
                LAPIC_ICR_TRIGGER_EDGE);

    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

void lapic_send_ipi_self(uint32_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                (vector & 0xFFu) |
                LAPIC_ICR_DELIVERY_FIXED |
                LAPIC_ICR_SHORTHAND_SELF |
                LAPIC_ICR_LEVEL_ASSERT |
                LAPIC_ICR_TRIGGER_EDGE);

    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

void lapic_send_ipi_others(uint32_t vector)
{
    lapic_write(LAPIC_ICR_HIGH, 0);
    lapic_write(LAPIC_ICR_LOW,
                (vector & 0xFFu) |
                LAPIC_ICR_DELIVERY_FIXED |
                LAPIC_ICR_SHORTHAND_OTHERS |
                LAPIC_ICR_LEVEL_ASSERT |
                LAPIC_ICR_TRIGGER_EDGE);

    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_DELIVERY_PENDING) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}
/*===OmniBridgeOs/kernel/arch/x64/lapic.c 结束===*/