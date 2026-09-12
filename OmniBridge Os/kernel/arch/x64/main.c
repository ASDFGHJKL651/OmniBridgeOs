//测试
#include "selftest.h"
#include "stress.h"
//
#include "boot.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "pic.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "slab.h"
#include "spinlock.h"
#include "percpu.h"
#include "printk.h"
#include "serial.h"

#ifdef OB_QEMU_EXIT
extern void qemu_exit(uint32_t code);
#endif

/* entry_kernel.S 导出，用作 TSS.rsp0 的内核栈顶 */
extern uint8_t kernel_stack_top[];

static void banner(void)
{
    printk("Hello Kernel\n");
}

/* 由 entry_kernel.S 调用；RCX = UEFI 传过来的 &g_boot_info（MS ABI 参数 1） */
void _kstart_c(struct ob_boot_info *bi)
{
    /* UEFI 侧 boot_info 位于 UEFI 镜像 BSS，随时可能被覆盖，
     * 第一件事就是整体复制到内核自己的 BSS。 */
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

    /* ---------- 第 4 步：GDT / TSS / IDT / PIC / SYSCALL ---------- */
    gdt_init();                                        /* 加载 7 项 GDT（含 TSS 描述符） */
    tss_init((uint64_t)(uintptr_t)kernel_stack_top);   /* 设置 rsp0 + ltr 0x28 */
    idt_init();                                        /* 0-31 异常 + 32-47 IRQ */
    pic_init();                                        /* 8259 重映射，屏蔽全部 IRQ */
    pit_init(100);                                     /* PIT 100 Hz -> IRQ0 */
    pic_unmask(0);                                     /* 仅打开 IRQ0 定时器 */
    syscall_init();                                    /* EFER.SCE / STAR / LSTAR / FMASK */

    /* ---------- 第 5 步：内存子系统（SMP-safe） ---------- */
    percpu_init();                                     /* 每 CPU 变量基础设施 */
    pmm_init(bi);                                      /* 伙伴系统（自旋锁保护） */
    vmm_init();                                        /* 4 级分页 */
    slab_init();                                       /* SLAB 子系统 */
    kmalloc_init();                                    /* kmalloc 大小类（基于 SLAB） */

    banner();

    /* ---------- 极简启动自检：kmalloc/kzalloc 往返 ---------- */
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

    /* ---------- 第 5 步压力测试（编译期开关 OB_STRESS_LEVEL） ---------- */
#if OB_STRESS_LEVEL != OB_STRESS_NONE
    {
        int rc = stress_run(OB_STRESS_LEVEL);
        if (rc != 0) {
            printk(KERN_ERR "[KRN] stress test (level %d) failed rc=%d\n",
                   OB_STRESS_LEVEL, rc);
            goto fail;
        }
    }
#endif

    #if OB_SELFTEST_STEP != OB_SELFTEST_NONE
        serial_printf("[KRN] running selftest step %d\n", OB_SELFTEST_STEP);
        selftest_run(OB_SELFTEST_STEP);
        serial_printf("[KRN] selftest step %d returned to _kstart_c\n",
                    OB_SELFTEST_STEP);
    #endif

    /* ---------- 打开中断，观察 PIT tick ---------- */
    __asm__ __volatile__("sti");
    printk("[KRN] interrupts enabled, waiting for PIT ticks...\n");

    /* 约 1 秒（100 Hz 下 100 次 hlt 就够，多 hlt 一些更保险） */
    for (int i = 0; i < 300; ++i) {
        __asm__ __volatile__("hlt");
    }

    printk("[KRN] PIT observed, entering idle loop\n");

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