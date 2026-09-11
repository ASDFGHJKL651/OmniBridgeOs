#include "gdt.h"
#include "printk.h"

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

static struct gdt_entry g_gdt[5];
static struct gdt_ptr   g_gdtp;

static void set_gate(int i, uint8_t access, uint8_t gran)
{
    g_gdt[i].limit_low   = 0xFFFF;
    g_gdt[i].base_low    = 0x0000;
    g_gdt[i].base_mid    = 0x00;
    g_gdt[i].access      = access;
    g_gdt[i].granularity = gran;
    g_gdt[i].base_high   = 0x00;
}

extern void gdt_flush(uint64_t gdtp_addr);

void gdt_init(void)
{
    /* 0: null */
    for (int i = 0; i < 6; ++i) {
        ((uint8_t *)&g_gdt[0])[i] = 0;
    }
    set_gate(0, 0x00, 0x00);

    /* 1: kernel code  (0x9A = present|DPL0|code|read, 0xA0 = long mode) */
    set_gate(1, 0x9A, 0xA0);
    /* 2: kernel data  (0x92 = present|DPL0|data|write) */
    set_gate(2, 0x92, 0xA0);
    /* 3: user code 32 (兼容位，未用) */
    set_gate(3, 0xFA, 0xA0);
    /* 4: user data */
    set_gate(4, 0xF2, 0xA0);

    g_gdtp.limit = sizeof(g_gdt) - 1;
    g_gdtp.base  = (uint64_t)&g_gdt[0];

    gdt_flush((uint64_t)&g_gdtp);
    printk("[GDT] loaded\n");
}