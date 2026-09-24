#include "gdt.h"
#include "tss.h"
#include "printk.h"
#include "serial.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* 7 个条目：0..4 普通段，5/6 = 16 字节 TSS 描述符 */
static struct gdt_entry g_gdt[7];
static struct gdt_ptr   g_gdtp;

extern void gdt_flush(uint64_t gdtp_addr);

static void set_gate(int i, uint8_t access, uint8_t gran)
{
    g_gdt[i].limit_low   = 0xFFFF;
    g_gdt[i].base_low    = 0x0000;
    g_gdt[i].base_mid    = 0x00;
    g_gdt[i].access      = access;
    g_gdt[i].granularity = gran;
    g_gdt[i].base_high   = 0x00;
}

/* 64 位 TSS 描述符（16 字节，占 g_gdt[i] 与 g_gdt[i+1]） */
static void set_tss_descriptor(int i, uint64_t base, uint32_t limit)
{
    /* 低 8 字节 */
    g_gdt[i].limit_low   = (uint16_t)(limit & 0xFFFF);
    g_gdt[i].base_low    = (uint16_t)(base & 0xFFFF);
    g_gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[i].access      = 0x89;    /* P=1, DPL=0, S=0, type=1001b */
    g_gdt[i].granularity = (uint8_t)((limit >> 16) & 0x0F);
    g_gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFF);

    /* 高 8 字节：base[63:32]，其余为 0 */
    g_gdt[i + 1].limit_low   = (uint16_t)((base >> 32) & 0xFFFF);
    g_gdt[i + 1].base_low    = (uint16_t)((base >> 48) & 0xFFFF);
    g_gdt[i + 1].base_mid    = 0;
    g_gdt[i + 1].access      = 0;
    g_gdt[i + 1].granularity = 0;
    g_gdt[i + 1].base_high   = 0;
}

void gdt_init(void)
{
    /* 0：null 描述符 */
    for (int i = 0; i < 8; ++i) ((uint8_t *)&g_gdt[0])[i] = 0;

    /* 1..4 普通段
     * 修复：交换用户数据段与用户代码段，使 SYSRET 能正确加载。
     * 索引 1: kernel code  0x08
     * 索引 2: kernel data  0x10
     * 索引 3: user data    0x18 | 3 = 0x1B
     * 索引 4: user code    0x20 | 3 = 0x23
     */
    set_gate(1, 0x9A, 0xA0);   /* kernel code */
    set_gate(2, 0x92, 0xA0);   /* kernel data */
    set_gate(3, 0xF2, 0xA0);   /* user data  (0x1B) */
    set_gate(4, 0xFA, 0xA0);   /* user code  (0x23) */

    /* 5/6：TSS 描述符 */
    struct tss_entry *t = tss_get();
    set_tss_descriptor(5, (uint64_t)(uintptr_t)t, (uint32_t)(sizeof(*t) - 1));

    g_gdtp.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdtp.base  = (uint64_t)(uintptr_t)&g_gdt[0];

    gdt_flush((uint64_t)(uintptr_t)&g_gdtp);

    serial_printf("[GDT] loaded 7 entries (null|kcode|kdata|udata|ucode|TSS.lo|TSS.hi)\n");
    serial_printf("[GDT] TSS base=0x%llx limit=0x%x sel=0x28\n",
                  (unsigned long long)(uint64_t)(uintptr_t)t,
                  (unsigned)(sizeof(*t) - 1));
}