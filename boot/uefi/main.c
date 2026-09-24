/*===OmniBridgeOs/boot/uefi/main.c===*/
#include "uefi_min.h"
#include "file.h"
#include "elf.h"
#include "page.h"
#include "serial.h"
#include "../../kernel/arch/x64/boot.h"

#ifdef OB_QEMU_EXIT
extern void qemu_exit(uint32_t code);
#endif

extern void uefi_jump_to_kernel(void *boot_info, void *entry_va);

/* ---------- BSS：ExitBootServices 之后仍然可访问 ---------- */
static struct ob_boot_info g_boot_info;
static uint8_t g_memory_map_buf[32768];

/* 全局 SystemTable，供 file.c 里 efi_get_bs 使用 */
static EFI_SYSTEM_TABLE *g_st;

EFI_STATUS efi_get_bs(EFI_BOOT_SERVICES **out)
{
    if (!g_st || !g_st->BootServices) return EFI_LOAD_ERROR;
    *out = g_st->BootServices;
    return EFI_SUCCESS;
}

static void fail_and_halt(const char *msg, EFI_STATUS st)
{
    serial_printf("[UEFI] FATAL: %s (status=0x%llx)\n", msg,
                  (unsigned long long)st);
#ifdef OB_QEMU_EXIT
    qemu_exit(1);
#endif
    for (;;) { __asm__ __volatile__("hlt"); }
}

/* kernel.elf 路径（UTF-16LE，以 0 结尾） */
static CHAR16 g_kernel_path[] = {
    '\\','k','e','r','n','e','l','.','e','l','f', 0
};

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    serial_init();
    serial_printf("Knot booting\n");
    serial_printf("OmniBridge OS UEFI boot stub v0.3\n");
    serial_printf("Kernel codename: Knot, userspace codename: Tide\n");

    if (!SystemTable || !SystemTable->BootServices) {
        fail_and_halt("no BootServices", EFI_LOAD_ERROR);
    }
    g_st = SystemTable;
    EFI_BOOT_SERVICES *bs = SystemTable->BootServices;

    /* 人工必须审查：EFI_BOOT_SERVICES 偏移断言 */
    {
        UINTN off_open  = (UINTN)((uint8_t *)&bs->OpenProtocol   - (uint8_t *)bs);
        UINTN off_locate= (UINTN)((uint8_t *)&bs->LocateProtocol - (uint8_t *)bs);
        UINTN off_exit  = (UINTN)((uint8_t *)&bs->ExitBootServices - (uint8_t *)bs);
        if (off_open != 0x118 || off_locate != 0x140 || off_exit != 0xE8) {
            fail_and_halt("EFI_BOOT_SERVICES layout mismatch", EFI_LOAD_ERROR);
        }
    }

    /* ---------- 1) 从 ESP 读取 kernel.elf ---------- */
    void *elf_buf = 0;
    UINTN elf_size = 0;
    EFI_STATUS st = uefi_read_file(ImageHandle, g_kernel_path, &elf_buf, &elf_size);
    if (EFI_ERROR(st)) fail_and_halt("read kernel.elf", st);
    serial_printf("[UEFI] kernel.elf loaded, %llu bytes\n",
                  (unsigned long long)elf_size);

    /* ---------- 2) 解析 ELF，按 p_paddr 复制到物理内存 ----------
     *
     * ★ 第 18C 步修复：
     *   elf_load 现在额外输出 kernel_phys_end（所有 PT_LOAD 段的
     *   p_paddr + p_memsz 的最大值）。该值被写入 boot_info，供内核
     *   pmm_init 排除内核镜像物理范围，避免用户程序覆写内核 BSS。
     */
    void *entry_va = 0;
    uint64_t kernel_lma = 0;
    uint64_t kernel_phys_end = 0;
    st = elf_load(elf_buf, elf_size,
                  OB_KERNEL_VMA,
                  &entry_va, &kernel_lma, &kernel_phys_end);
    if (EFI_ERROR(st)) fail_and_halt("elf_load", st);
    serial_printf("[UEFI] ELF entry=%p LMA=%p phys_end=0x%llx\n",
                  entry_va, (void *)(uintptr_t)kernel_lma,
                  (unsigned long long)kernel_phys_end);

    if (kernel_lma != OB_KERNEL_LMA) {
        fail_and_halt("kernel LMA mismatch (expect OB_KERNEL_LMA)",
                      EFI_LOAD_ERROR);
    }

    /* ★ 第 18C 步：填充内核物理范围字段。
     *
     * 人工必须审查：
     *   - kernel_phys_start = 内核加载基址（OB_KERNEL_LMA）。
     *   - kernel_phys_end   = 所有 PT_LOAD 段 p_paddr + p_memsz 的最大值。
     *   - 若 kernel_phys_end == 0（理论上不应发生），内核 pmm_init 会
     *     使用保守默认范围 [0x200000, 0x400000)。 */
    g_boot_info.kernel_phys_start = kernel_lma;
    g_boot_info.kernel_phys_end   = kernel_phys_end;

    /* ---------- 3) 获取内存图 ---------- */
    UINTN map_size = sizeof(g_memory_map_buf);
    UINTN map_key  = 0;
    UINTN desc_size = 0;
    uint32_t desc_ver = 0;
    st = bs->GetMemoryMap(&map_size,
                          (EFI_MEMORY_DESCRIPTOR *)g_memory_map_buf,
                          &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(st)) fail_and_halt("GetMemoryMap", st);

    g_boot_info.memory_map_size     = map_size;
    g_boot_info.descriptor_size     = desc_size;
    g_boot_info.descriptor_version  = desc_ver;

    UINTN count = map_size / desc_size;
    if (count > OB_MAX_MEMORY_MAP_ENTRIES) count = OB_MAX_MEMORY_MAP_ENTRIES;
    g_boot_info.entry_count = (uint32_t)count;

    for (UINTN i = 0; i < count; ++i) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR *)(g_memory_map_buf + i * desc_size);
        g_boot_info.memory_map[i].type            = d->Type;
        g_boot_info.memory_map[i].physical_start  = d->PhysicalStart;
        g_boot_info.memory_map[i].virtual_start   = d->VirtualStart;
        g_boot_info.memory_map[i].number_of_pages = d->NumberOfPages;
        g_boot_info.memory_map[i].attribute       = d->Attribute;
    }

    /* ---------- 4) 建立初始页表（★ 不切换 CR3） ----------
     *
     * ★ 关键修复：
     *   原实现会在这里执行 `mov %cr3`，导致 OVMF 内部残留事件
     *   （尤其是 VirtIO 网卡 UEFI 驱动）在自定义页表下运行，
     *   访问 MMIO 时触发 #PF。
     *
     *   现在改为：仅构造页表，把 PML4 的物理地址返回，
     *   由本函数在 ExitBootServices 成功之后再手动切换。
     */
    void *pml4_phys = 0;
    uefi_build_page_tables(kernel_lma, &pml4_phys);
    serial_printf("[UEFI] page tables built, pml4=0x%llx\n",
                  (unsigned long long)pml4_phys);

    /* ---------- 5) 退出 BootServices ---------- */
    st = bs->ExitBootServices(ImageHandle, map_key);
    if (EFI_ERROR(st)) {
        /* 内存图可能变化，重读一次再试 */
        map_size = sizeof(g_memory_map_buf);
        st = bs->GetMemoryMap(&map_size,
                              (EFI_MEMORY_DESCRIPTOR *)g_memory_map_buf,
                              &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(st)) fail_and_halt("GetMemoryMap(retry)", st);
        st = bs->ExitBootServices(ImageHandle, map_key);
        if (EFI_ERROR(st)) fail_and_halt("ExitBootServices", st);
    }
    serial_printf("[UEFI] Boot Services exited\n");

    /* ---------- 6) 切换 CR3，然后跳转到内核 VMA ---------- */
    serial_printf("[UEFI] handoff to kernel at %p, entries=%u\n",
                  entry_va, (unsigned)g_boot_info.entry_count);

    /*
     * ★ 关键：CR3 切换必须放在 ExitBootServices 之后。
     *   到此为止 UEFI 侧的所有异步事件/驱动回调都已停止，不会再有人
     *   在错误的页表下访问内存。
     *
     *   切换后当前 RIP / RSP / 数据段 / 栈（全部位于 0..4GB 恒等映射内）
     *   仍然有效，随后即跳入内核高半区。
     */
    __asm__ __volatile__(
        "mov %0, %%cr3\n\t"
        :
        : "r"((uint64_t)(uintptr_t)pml4_phys)
        : "memory");

    uefi_jump_to_kernel(&g_boot_info, entry_va);

    for (;;) { __asm__ __volatile__("hlt"); }
}
/*===OmniBridgeOs/boot/uefi/main.c 结束===*/