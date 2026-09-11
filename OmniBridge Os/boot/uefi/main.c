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

    /* ---------- 2) 解析 ELF，按 p_paddr 复制到物理内存 ---------- */
    void *entry_va = 0;
    uint64_t kernel_lma = 0;
    st = elf_load(elf_buf, elf_size,
                  OB_KERNEL_VMA,           /* ← 新增 */
                  &entry_va, &kernel_lma);
    if (EFI_ERROR(st)) fail_and_halt("elf_load", st);
    serial_printf("[UEFI] ELF entry=%p LMA=%p\n",
                  entry_va, (void *)(uintptr_t)kernel_lma);

    /* 与 boot.h 中的 OB_KERNEL_LMA 严格比对 */
    if (kernel_lma != OB_KERNEL_LMA) {
        fail_and_halt("kernel LMA mismatch (expect OB_KERNEL_LMA)",
                      EFI_LOAD_ERROR);
    }

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

    /* ---------- 4) 建立初始页表并加载 CR3 ---------- */
    uefi_build_page_tables(kernel_lma);
    serial_printf("[UEFI] page tables built\n");

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

    /* ---------- 6) 跳转到内核 VMA ---------- */
    serial_printf("[UEFI] handoff to kernel at %p, entries=%u\n",
                  entry_va, (unsigned)g_boot_info.entry_count);

    /* 自此不再调用任何 Boot Services。
     * 串口输出仍然可用（端口 I/O 直接操作 0x3F8）。 */
    uefi_jump_to_kernel(&g_boot_info, entry_va);

    for (;;) { __asm__ __volatile__("hlt"); }
}