#include "boot.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "printk.h"
#include "serial.h"

#ifdef OB_QEMU_EXIT
extern void qemu_exit(uint32_t code);
#endif

static void banner(void)
{
    printk("Hello Kernel\n");
}

/* 由 entry_kernel.S 调用；RDI = UEFI 传过来的 &g_boot_info */
void _kstart_c(struct ob_boot_info *bi)
{
    /* 人工必须审查：
     *   UEFI 传进来的 boot_info 位于 UEFI 镜像 BSS（物理 <4GB）。
     *   一旦 pmm_init 把该物理内存标记为可用，它随时可能被覆盖。
     *   因此第一件事是整体复制到内核自己的 BSS。 */
    static struct ob_boot_info local_bi;
    if (bi) {
        uint8_t *src = (uint8_t *)bi;
        uint8_t *dst = (uint8_t *)&local_bi;
        for (size_t i = 0; i < sizeof(local_bi); ++i) dst[i] = src[i];
        bi = &local_bi;
    }

    if (!bi) {
        serial_printf("[KRN] no boot_info, halted\n");
        goto halt;
    }

    gdt_init();
    idt_init();
    pmm_init(bi);
    vmm_init();
    kmalloc_init();

    banner();

    void *p = kmalloc(64);
    if (!p) {
        printk(KERN_ERR "kmalloc(64) failed\n");
        goto fail;
    }
    for (int i = 0; i < 64; ++i) ((uint8_t *)p)[i] = (uint8_t)i;
    kfree(p);

    void *q = kzalloc(128);
    if (!q) {
        printk(KERN_ERR "kzalloc(128) failed\n");
        goto fail;
    }
    kfree(q);

    printk("kmalloc OK\n");

#ifdef OB_QEMU_EXIT
    qemu_exit(0);
#endif
    goto halt;

fail:
#ifdef OB_QEMU_EXIT
    qemu_exit(1);
#endif
halt:
    for (;;) { __asm__ __volatile__("hlt"); }
}