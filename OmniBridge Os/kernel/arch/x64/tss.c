#include "tss.h"
#include "serial.h"

/* 全局 TSS，位于 .bss。gdt.c 通过 tss_get() 取其地址编码到 GDT[5..6]。 */
static struct tss_entry g_tss;

struct tss_entry *tss_get(void)
{
    return &g_tss;
}

void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp0 = rsp0;
}

/*
 * 载入 TR（Task Register）。
 * 选择子 0x28 指向 GDT 条目 5。
 *
 * 说明（人工必须审查）：
 *   这里用 "r" 约束把选择子送进 16 位寄存器，避免 "i" 约束在
 *   AT&T/COFF 下对 $ 前缀与立即数大小写的不确定性。
 */
static void ltr_load(uint16_t selector)
{
    __asm__ __volatile__("ltr %0"
                         :: "r"(selector)
                         : "memory");
}

void tss_init(uint64_t rsp0)
{
    /* 1) 整体清零 */
    uint8_t *p = (uint8_t *)&g_tss;
    for (unsigned i = 0; i < sizeof(g_tss); ++i) p[i] = 0;

    /* 2) 内核栈顶 */
    g_tss.rsp0 = rsp0;

    /* 3) I/O 位图基址 = sizeof(tss)：无位图，所有 I/O 由 IOPL 检查控制 */
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);

    /* 4) 加载 TR：选择子 0x28 = GDT[5] */
    ltr_load(0x28);

    serial_printf("[TSS] initialized, rsp0=0x%llx, ltr sel=0x28\n",
                  (unsigned long long)rsp0);
}