/*===OmniBridgeOs/kernel/arch/x64/ioapic.c===*/
#include "ioapic.h"
#include "lapic.h"
#include "serial.h"
#include "vmm.h"                    /* ★ 第 18C 步：DIRECTMAP_BASE */

/*
 * 人工必须审查：
 *   - ★ 第 18C 步：IOAPIC MMIO 通过 DirectMap 访问。
 *     原因：vmm_create_address_space 不再复制 PML4[0]，恒等映射
 *     在内核态（切换 CR3 后）不再可用。
 *   - IOREGSEL 与 IOWIN 必须在同一 4K 页；QEMU 的 IOAPIC 默认在
 *     0xFEC00000 满足该要求。
 *   - 访问 MMIO 时 IOREGSEL 写入后再访问 IOWIN；用 volatile + 读回
 *     确保顺序。
 */

static volatile uint8_t *g_ioapic_base = 0;
static uint32_t g_ioapic_max_redir = 0;

static void ioapic_select(uint32_t reg)
{
    volatile uint32_t *sel = (volatile uint32_t *)(g_ioapic_base + 0x00);
    *sel = reg;
    (void)*sel;
}

static uint32_t ioapic_read(uint32_t reg)
{
    ioapic_select(reg);
    volatile uint32_t *win = (volatile uint32_t *)(g_ioapic_base + 0x10);
    return *win;
}

static void ioapic_write(uint32_t reg, uint32_t val)
{
    ioapic_select(reg);
    volatile uint32_t *win = (volatile uint32_t *)(g_ioapic_base + 0x10);
    *win = val;
}

void ioapic_init(void)
{
    /*
     * ★ 第 18C 步：通过 DirectMap 访问 IOAPIC。
     *   原值 (volatile uint8_t *)IOAPIC_DEFAULT_BASE 依赖恒等映射，
     *   在用户 PML4 切换后失效。
     */
    g_ioapic_base = (volatile uint8_t *)(DIRECTMAP_BASE +
                                          IOAPIC_DEFAULT_BASE);

    uint32_t id  = ioapic_read(IOAPIC_REG_ID);
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);

    g_ioapic_max_redir = ((ver >> 16) & 0xFFu) + 1u;

    serial_printf("[IOAPIC] id=0x%x version=0x%x redir_entries=%u "
                  "vaddr=0x%llx\n",
                  (unsigned)(id >> 24),
                  (unsigned)(ver & 0xFFu),
                  (unsigned)g_ioapic_max_redir,
                  (unsigned long long)(uintptr_t)g_ioapic_base);

    /* 屏蔽所有重定向项 */
    for (uint32_t i = 0; i < g_ioapic_max_redir; ++i) {
        uint32_t low_reg  = IOAPIC_REG_REDTBL0 + 2u * i;
        uint32_t high_reg = low_reg + 1u;
        ioapic_write(low_reg,  (1u << 16));   /* masked */
        ioapic_write(high_reg, 0);
    }
}

uint32_t ioapic_version(void)
{
    return ioapic_read(IOAPIC_REG_VER);
}

void ioapic_set_redir(uint8_t irq, uint8_t vector, uint8_t apic_id)
{
    if (irq >= g_ioapic_max_redir) return;

    uint32_t low_reg  = IOAPIC_REG_REDTBL0 + 2u * irq;
    uint32_t high_reg = low_reg + 1u;

    uint32_t low = (uint32_t)vector
                 | (0u << 8)
                 | (0u << 11)
                 | (0u << 13)
                 | (0u << 15)
                 | (0u << 16);

    uint32_t high = ((uint32_t)apic_id & 0xFFu) << 24;

    ioapic_write(high_reg, high);
    ioapic_write(low_reg,  low);
}

void ioapic_mask(uint8_t irq)
{
    if (irq >= g_ioapic_max_redir) return;
    uint32_t low_reg = IOAPIC_REG_REDTBL0 + 2u * irq;
    uint32_t low = ioapic_read(low_reg);
    low |= (1u << 16);
    ioapic_write(low_reg, low);
}

void ioapic_unmask(uint8_t irq)
{
    if (irq >= g_ioapic_max_redir) return;
    uint32_t low_reg = IOAPIC_REG_REDTBL0 + 2u * irq;
    uint32_t low = ioapic_read(low_reg);
    low &= ~(1u << 16);
    ioapic_write(low_reg, low);
}
/*===OmniBridgeOs/kernel/arch/x64/ioapic.c 结束===*/