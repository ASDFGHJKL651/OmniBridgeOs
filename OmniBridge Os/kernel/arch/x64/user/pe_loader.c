/*===OmniBridgeOs/kernel/arch/x64/user/pe_loader.c===*/
/*
 * PE32+ 加载器实现。
 *
 * 人工必须审查（关键安全不变量）：
 *   1) 所有 RVA -> VA 运算使用 uint64_t，防止整数溢出。
 *   2) 每个 Section 的 PointerToRawData + SizeOfRawData <= size。
 *   3) 每个 Section 的 VirtualAddress + VirtualSize <= SizeOfImage。
 *   4) 拒绝 DLL 与非 AMD64 与非 PE32+。
 *   5) IAT 写入必须通过 vmm_get_pte + DIRECTMAP_BASE，
 *      绝不能直接解引用用户 VA。
 *   6) 未解析的 API 必须失败，不假装成功。
 */
#include "pe_loader.h"
#include "user.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "kmalloc.h"
#include "audit.h"

/* 保护上限：SizeOfImage <= 512 MiB */
#define PE_MAX_IMAGE_SIZE    (512u * 1024u * 1024u)
#define PE_MAX_SECTIONS      96

static uint32_t info_size_opt(const uint8_t *img);

/* ---------- 用户态内存访问辅助 ---------- */

static uint64_t *pe_pml4_of(struct task_t *t)
{
    struct user_ctx *c = user_get_ctx(t);
    return c ? c->pml4 : 0;
}

/* 写用户态 u64：通过 vmm_get_pte 反查物理页。 */
static int pe_write_u64(uint64_t *pml4, uint64_t va, uint64_t val)
{
    uint64_t *pte = vmm_get_pte(pml4, va);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -1;
    uint64_t pa = *pte & PTE_ADDR_MASK;
    uint64_t off = va & 0xFFFu;
    uint64_t *dst = (uint64_t *)(uintptr_t)(DIRECTMAP_BASE + pa + off);
    *dst = val;
    return 0;
}

/* 在用户态映射一段 [va_start, va_end)，页对齐；新页清零。
 *
 * 人工必须审查（第 20 步 v2）：
 *   - 所有新页 PTE = P | RW | U（不加 NX）。
 *   - 精确实现应按段 Characteristics 分别设 RX/R/RW；本步暂统一 RWX，
 *     由后续步骤细化。
 *   - 已有 PTE 时跳过，避免与已映射区域（如 stub 页、PEB/TEB）冲突。 */
static int pe_map_range(struct task_t *t, uint64_t va_start, uint64_t va_end)
{
    if (va_end <= va_start) return 0;
    if (!user_range_ok(va_start, va_end - va_start)) return -1;

    uint64_t *pml4 = pe_pml4_of(t);
    if (!pml4) return -1;

    uint64_t start_page = va_start & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end_page   = (va_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    for (uint64_t va = start_page; va < end_page; va += PAGE_SIZE) {
        uint64_t *pte = vmm_get_pte(pml4, va);
        if (pte && (*pte & PTE_PRESENT)) continue;

        struct page *pg = pmm_alloc_pages(0);
        if (!pg) return -1;
        uint64_t pa = page_to_phys(pg);

        int rc = vmm_map_page(pml4, va, pa,
                              PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        if (rc != 0) { pmm_free_pages(pg, 0); return -1; }

        uint8_t *dst = (uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
        for (uint64_t k = 0; k < PAGE_SIZE; ++k) dst[k] = 0;
    }
    return 0;
}

/* 从用户态 VA 读到内核缓冲。 */
static int pe_read_user(uint64_t *pml4, uint64_t va, void *dst, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint64_t done = 0;
    while (done < n) {
        uint64_t cur = va + done;
        uint64_t *pte = vmm_get_pte(pml4, cur);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -1;
        uint64_t pa  = *pte & PTE_ADDR_MASK;
        uint64_t off = cur & 0xFFFu;
        uint64_t take = PAGE_SIZE - off;
        if (take > (n - done)) take = n - done;
        const uint8_t *src = (const uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa + off);
        for (uint64_t k = 0; k < take; ++k) d[done + k] = src[k];
        done += take;
    }
    return 0;
}

/* 从用户态 VA 写内核缓冲到用户空间。 */
static int pe_write_user(uint64_t *pml4, uint64_t va, const void *src, uint64_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    uint64_t done = 0;
    while (done < n) {
        uint64_t cur = va + done;
        uint64_t *pte = vmm_get_pte(pml4, cur);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return -1;
        uint64_t pa  = *pte & PTE_ADDR_MASK;
        uint64_t off = cur & 0xFFFu;
        uint64_t take = PAGE_SIZE - off;
        if (take > (n - done)) take = n - done;
        uint8_t *d = (uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa + off);
        for (uint64_t k = 0; k < take; ++k) d[k] = s[done + k];
        done += take;
    }
    return 0;
}

/* ---------- 头部解析 ---------- */

int pe_detect(const void *buf, uint64_t size)
{
    if (!buf || size < sizeof(struct pe_dos_hdr)) return 0;

    const uint8_t *p = (const uint8_t *)buf;
    if (p[0] != 'M' || p[1] != 'Z') return 0;

    if (size < 0x40) return -1;
    uint32_t e_lfanew = *(const uint32_t *)(const void *)(p + 0x3C);
    if (e_lfanew < sizeof(struct pe_dos_hdr)) return -1;
    if ((uint64_t)e_lfanew + 4 + sizeof(struct pe_coff_hdr) > size) return -1;

    uint32_t sig = *(const uint32_t *)(const void *)(p + e_lfanew);
    if (sig != PE_NT_SIGNATURE) return -1;

    const struct pe_coff_hdr *coff =
        (const struct pe_coff_hdr *)(const void *)(p + e_lfanew + 4);
    if (coff->Machine != PE_MACHINE_AMD64) return -1;
    if (!(coff->Characteristics & PE_FILE_EXECUTABLE)) return -1;
    if (coff->Characteristics & PE_FILE_DLL) return -1;

    uint64_t opt_off = (uint64_t)e_lfanew + 4 + sizeof(struct pe_coff_hdr);
    if (opt_off + 2 > size) return -1;
    uint16_t magic = *(const uint16_t *)(const void *)(p + opt_off);
    if (magic != PE_OPT_MAGIC_PE32P) return -1;

    return 1;
}

int pe_parse_info(const void *buf, uint64_t size, struct pe_info *out)
{
    if (!buf || !out) return -22;
    int d = pe_detect(buf, size);
    if (d != 1) return (d == 0) ? -22 : -5;

    const uint8_t *p = (const uint8_t *)buf;
    uint32_t e_lfanew = *(const uint32_t *)(const void *)(p + 0x3C);

    const struct pe_coff_hdr *coff =
        (const struct pe_coff_hdr *)(const void *)(p + e_lfanew + 4);

    if (coff->NumberOfSections == 0 || coff->NumberOfSections > PE_MAX_SECTIONS)
        return -5;
    if (coff->SizeOfOptionalHeader < sizeof(struct pe_opt_hdr64) + 16 * 8)
        return -5;

    uint64_t opt_off = (uint64_t)e_lfanew + 4 + sizeof(struct pe_coff_hdr);
    const struct pe_opt_hdr64 *opt =
        (const struct pe_opt_hdr64 *)(const void *)(p + opt_off);

    if (opt->Magic != PE_OPT_MAGIC_PE32P) return -5;
    if (opt->SizeOfImage == 0 || opt->SizeOfImage > PE_MAX_IMAGE_SIZE) return -5;
    if (opt->SectionAlignment != 0x1000) {
        serial_printf("[PE] WARN: SectionAlignment=0x%x (expect 0x1000)\n",
                      (unsigned)opt->SectionAlignment);
    }
    if (opt->AddressOfEntryPoint >= opt->SizeOfImage) return -5;

    out->entry_rva       = opt->AddressOfEntryPoint;
    out->image_base_pref = opt->ImageBase;
    out->size_of_image   = opt->SizeOfImage;
    out->size_of_headers = opt->SizeOfHeaders;
    out->section_alignment = opt->SectionAlignment;
    out->file_alignment    = opt->FileAlignment;
    out->subsystem         = opt->Subsystem;
    out->dll_characteristics = opt->DllCharacteristics;
    out->stack_reserve = opt->SizeOfStackReserve;
    out->stack_commit  = opt->SizeOfStackCommit;
    out->heap_reserve  = opt->SizeOfHeapReserve;
    out->heap_commit   = opt->SizeOfHeapCommit;
    out->num_sections  = coff->NumberOfSections;
    out->_pad          = 0;
    return 0;
}

/* ---------- 加载段 ---------- */

int pe_load_into_task(struct task_t *t,
                      const uint8_t *img, uint64_t size,
                      uint64_t *entry_va, uint64_t *image_base,
                      uint32_t *size_of_image)
{
    if (!t || !img || !entry_va || !image_base || !size_of_image) return -22;

    struct pe_info info;
    int rc = pe_parse_info(img, size, &info);
    if (rc != 0) {
        serial_printf("[PE] parse_info failed rc=%d\n", rc);
        return rc;
    }

    uint64_t base_pref = info.image_base_pref;

    /* ------------------------------------------------------------------
     * 人工必须审查（第 20 步 v2）：ImageBase 必须落在 PML4[1..255]
     *
     *   x86-64 页表遍历要求每一级 PML4/PDPT/PD/PT 的 U 位都为 1，
     *   CPL=3 代码才能访问。用户 PML4[0] 从内核主 PML4 复制而来
     *   （低恒等映射 0..4GB 用 2MB 大页，U=0），因此任何位于
     *   PML4[0]（0 ~ 512 GiB - 1）的 ImageBase 都会导致用户态
     *   #PF(I=1, P=1, U=1) —— 这是 win32_hello 首次失败的根本原因。
     *
     *   编译 PE 时必须传 -Wl,--image-base,0x8000000000（= 512 GiB）。
     *   若仍冲突则拒绝加载，不静默重定位（本步不实现重定位）。
     * ------------------------------------------------------------------ */
    uint64_t i4 = base_pref >> 39;
    if (i4 == 0) {
        serial_printf("[PE] FATAL: ImageBase=0x%llx falls in PML4[0] "
                      "(kernel identity map 0..4GB, U=0). "
                      "Rebuild PE with -Wl,--image-base,0x8000000000.\n",
                      (unsigned long long)base_pref);
        return -5;
    }
    if (i4 >= 256) {
        serial_printf("[PE] FATAL: ImageBase=0x%llx in kernel space\n",
                      (unsigned long long)base_pref);
        return -5;
    }
    if (!user_range_ok(base_pref, info.size_of_image)) {
        serial_printf("[PE] FATAL: ImageBase=0x%llx + SizeOfImage=0x%x "
                      "out of user space\n",
                      (unsigned long long)base_pref,
                      (unsigned)info.size_of_image);
        return -5;
    }

    uint64_t image_end = base_pref + info.size_of_image;
    if (image_end < base_pref) return -5;

    rc = pe_map_range(t, base_pref, image_end);
    if (rc != 0) {
        serial_printf("[PE] map_range failed [0x%llx,0x%llx)\n",
                      (unsigned long long)base_pref,
                      (unsigned long long)image_end);
        return rc;
    }

    /* 拷贝 PE 头（SizeOfHeaders 覆盖范围内的原始数据）。
     * 有些程序读 PEB->ImageBase 后回头读 PE 头，因此即使加载器不需要，
     * 也应保留。 */
    uint64_t header_copy = info.size_of_headers;
    if (header_copy > size) header_copy = size;
    uint64_t *pml4 = pe_pml4_of(t);
    if (!pml4) return -1;
    if (pe_write_user(pml4, base_pref, img, header_copy) != 0) return -5;

    /* 遍历 Section 表 */
    uint32_t e_lfanew = *(const uint32_t *)(const void *)(img + 0x3C);
    uint64_t sect_off = (uint64_t)e_lfanew + 4 +
                        sizeof(struct pe_coff_hdr) + info_size_opt(img);

    for (uint16_t i = 0; i < info.num_sections; ++i) {
        const struct pe_section_hdr *s =
            (const struct pe_section_hdr *)(const void *)
            (img + sect_off + (uint64_t)i * sizeof(struct pe_section_hdr));

        uint64_t raw_off = s->PointerToRawData;
        uint64_t raw_sz  = s->SizeOfRawData;
        uint64_t vsz     = s->VirtualSize;
        uint64_t rva     = s->VirtualAddress;

        if (raw_off + raw_sz < raw_off) return -5;
        if (raw_off + raw_sz > size) {
            serial_printf("[PE] section %u raw out of file "
                          "(off=0x%llx sz=0x%llx file_sz=0x%llx)\n",
                          (unsigned)i,
                          (unsigned long long)raw_off,
                          (unsigned long long)raw_sz,
                          (unsigned long long)size);
            return -5;
        }
        if (vsz < raw_sz) vsz = raw_sz;
        uint64_t va = base_pref + rva;
        if (va + vsz < va) return -5;
        if (va + vsz > image_end) {
            serial_printf("[PE] section %u exceeds image\n", (unsigned)i);
            return -5;
        }

        if (raw_sz > 0) {
            rc = pe_write_user(pml4, va, img + raw_off, raw_sz);
            if (rc != 0) return -5;
        }

        /* ------------------------------------------------------------------
         * 人工必须审查（第 20 步 v2）：
         *   内核 ob_vsnprintf 不支持精度修饰符（.N）。`%.8s` 会被解析为
         *   '%.' + '8' + 's' 三段字面输出，且不消费 s->Name 参数，
         *   导致后续 %x/%llx 全部错位一格（rva 打印出的是 s->Name 指针）。
         *   必须手工截断为 NUL 结尾字符串再 %s。
         * ------------------------------------------------------------------ */
        char sname[9];
        int ns = 0;
        for (; ns < 8; ++ns) {
            char c = s->Name[ns];
            if (c == '\0') break;
            sname[ns] = c;
        }
        sname[ns] = '\0';

        serial_printf("[PE] section '%s' rva=0x%x vsz=0x%llx raw=0x%llx\n",
                      sname, (unsigned)rva,
                      (unsigned long long)vsz,
                      (unsigned long long)raw_sz);
    }

    *entry_va      = base_pref + info.entry_rva;
    *image_base    = base_pref;
    *size_of_image = info.size_of_image;

    audit_event(AUDIT_EV_WIN32_LOAD, AUDIT_LVL_INFO, t->pid,
                base_pref, *entry_va, 0, "(pe-load)");

    serial_printf("[PE] loaded pid=%llu image_base=0x%llx entry=0x%llx "
                  "size=0x%x\n",
                  (unsigned long long)t->pid,
                  (unsigned long long)base_pref,
                  (unsigned long long)*entry_va,
                  (unsigned)info.size_of_image);
    return 0;
}

/* 计算 optional header 大小（含数据目录） */
static uint32_t info_size_opt(const uint8_t *img)
{
    uint32_t e_lfanew = *(const uint32_t *)(const void *)(img + 0x3C);
    const struct pe_coff_hdr *coff =
        (const struct pe_coff_hdr *)(const void *)(img + e_lfanew + 4);
    return coff->SizeOfOptionalHeader;
}

/* ---------- 导入表解析 ---------- */

static int str_casecmp_(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        ++a; ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int pe_resolve_imports(struct task_t *t,
                       const uint8_t *img, uint64_t size,
                       uint64_t image_base,
                       uint64_t (*stub_lookup)(const char *dll,
                                                const char *func))
{
    if (!t || !img || !stub_lookup) return -22;

    /*
     * 人工必须审查：
     *   - size 参数是本步为后续版本保留的接口位（用于将来校验
     *     ILT/IAT 的 RVA 是否落在文件范围内）。本步所有 RVA 通过
     *     image_base + RVA 走用户态映射访问，不再依赖文件大小，
     *     因此显式标记未使用。
     */
    (void)size;

    uint32_t e_lfanew = *(const uint32_t *)(const void *)(img + 0x3C);
    uint64_t opt_off  = (uint64_t)e_lfanew + 4 + sizeof(struct pe_coff_hdr);
    const struct pe_opt_hdr64 *opt =
        (const struct pe_opt_hdr64 *)(const void *)(img + opt_off);

    if (opt->NumberOfRvaAndSizes <= PE_DIR_IMPORT) return 0;

    const struct pe_data_dir *dirs =
        (const struct pe_data_dir *)(const void *)(opt + 1);
    uint32_t imp_rva = dirs[PE_DIR_IMPORT].VirtualAddress;
    uint32_t imp_sz  = dirs[PE_DIR_IMPORT].Size;
    if (imp_rva == 0 || imp_sz == 0) {
        serial_printf("[PE] no import directory\n");
        return 0;
    }

    uint64_t *pml4 = pe_pml4_of(t);
    if (!pml4) return -1;

    /* 遍历 import descriptors */
    uint64_t desc_va = image_base + imp_rva;
    for (uint64_t off = 0; off + sizeof(struct pe_import_desc) <= imp_sz;
         off += sizeof(struct pe_import_desc)) {
        struct pe_import_desc id;
        if (pe_read_user(pml4, desc_va + off, &id, sizeof(id)) != 0) return -5;
        if (id.Name == 0 && id.FirstThunk == 0 && id.OriginalFirstThunk == 0)
            break;  /* 全 0 结束 */

        /* DLL 名 */
        char dll_name[128];
        uint64_t name_va = image_base + id.Name;
        if (pe_read_user(pml4, name_va, dll_name, sizeof(dll_name) - 1) != 0)
            return -5;
        dll_name[sizeof(dll_name) - 1] = '\0';

        serial_printf("[PE] import: %s\n", dll_name);

        /* 遍历 ILT（查找表），绑定 IAT */
        uint32_t ilt_rva = id.OriginalFirstThunk ? id.OriginalFirstThunk
                                                 : id.FirstThunk;
        uint32_t iat_rva = id.FirstThunk;
        if (ilt_rva == 0 || iat_rva == 0) continue;

        for (uint32_t k = 0; ; ++k) {
            uint64_t ilt_va = image_base + ilt_rva + (uint64_t)k * 8;
            uint64_t ilt_ent = 0;
            if (pe_read_user(pml4, ilt_va, &ilt_ent, 8) != 0) return -5;
            if (ilt_ent == 0) break;

            /* 最高位为 1：ordinal 导入 —— 本步拒绝 */
            if (ilt_ent & 0x8000000000000000ULL) {
                serial_printf("[PE] ordinal imports not supported\n");
                return -38;
            }

            /* 名称导入：读 IMAGE_IMPORT_BY_NAME（Hint + Name） */
            uint64_t hdr_va = image_base + (ilt_ent & 0x7FFFFFFF);
            char fn_name[128];
            if (pe_read_user(pml4, hdr_va + 2, fn_name, sizeof(fn_name) - 1) != 0)
                return -5;
            fn_name[sizeof(fn_name) - 1] = '\0';

            uint64_t stub_va = stub_lookup(dll_name, fn_name);
            if (stub_va == 0) {
                serial_printf("[PE] unresolved import %s!%s\n",
                              dll_name, fn_name);
                return -2;
            }

            uint64_t iat_va = image_base + iat_rva + (uint64_t)k * 8;
            if (pe_write_u64(pml4, iat_va, stub_va) != 0) return -5;
            serial_printf("[PE]   %s -> stub 0x%llx\n",
                          fn_name, (unsigned long long)stub_va);
        }
    }

    serial_printf("[PE] IAT resolve OK\n");
    return 0;
}