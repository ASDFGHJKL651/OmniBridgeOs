/*===OmniBridgeOs/kernel/arch/x64/user/elf_loader.c===*/
#include "elf_loader.h"
#include "user.h"
#include "vfs.h"
#include "kmalloc.h"
#include "vmm.h"
#include "pmm.h"
#include "serial.h"
#include "sched.h"
#include "../obr.h"
/* ★ 第 19 步修复：linux_build_stack 中调用 rng_bytes()，必须包含头文件。
 *   缺少本行会在 -O2 -Wall -Wextra + C99 下产生
 *   "call to undeclared function 'rng_bytes'"
 *   错误（隐式函数声明已不是合法 C）。 */
#include "rng.h"

#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_JMPREL   23
#define DT_PLTRELSZ 2

static int g_dyn_inited = 0;

/* ★ 修复 10：全局库集合（用于跨库符号解析） */
static struct dyn_lib *g_dyn_libs[DYN_MAX_LIBS];
static uint32_t g_dyn_lib_count = 0;

void elf_loader_init(void)
{
    if (g_dyn_inited) return;
    g_dyn_inited = 1;
    g_dyn_lib_count = 0;
    for (uint32_t i = 0; i < DYN_MAX_LIBS; ++i) g_dyn_libs[i] = 0;
    serial_printf("[DYN] init: RELATIVE/64/GLOB_DAT/JUMP_SLOT supported\n");
}

/* ★ 修复 10：注册库 */
void dyn_register_lib(struct dyn_lib *lib)
{
    if (!lib) return;
    if (g_dyn_lib_count < DYN_MAX_LIBS) {
        g_dyn_libs[g_dyn_lib_count++] = lib;
        serial_printf("[DYN] registered lib '%s' (slot %u)\n",
                      lib->name, (unsigned)(g_dyn_lib_count - 1));
    }
}

/* ★ 修复 10：查找符号 */
int dyn_find_symbol(const char *name, uint64_t *out_addr)
{
    if (!name || !out_addr) return -1;
    for (uint32_t i = 0; i < g_dyn_lib_count; ++i) {
        struct dyn_lib *lib = g_dyn_libs[i];
        if (!lib) continue;
        for (uint32_t j = 0; j < lib->export_count; ++j) {
            const char *a = lib->exports[j].name;
            const char *b = name;
            int eq = 1;
            while (*a && *b) { if (*a != *b) { eq = 0; break; } ++a; ++b; }
            if (eq && *a == '\0' && *b == '\0') {
                *out_addr = lib->exports[j].addr;
                return 0;
            }
        }
    }
    return -1;
}

static int map_lib_obr(struct task_t *t, const struct obr_header *h,
                       const uint8_t *img, uint64_t size,
                       uint64_t base_va, struct dyn_lib *out);

static uint64_t *task_user_pml4_ptr(struct task_t *t)
{
    struct user_ctx *c = user_get_ctx(t);
    if (!c) return 0;
    return c->pml4;
}

/* 用户态 VA -> 物理 VA 的小工具 */
static int user_va_to_phys(uint64_t *pml4, uint64_t va, uint64_t *out_pa)
{
    uint64_t *pte = vmm_get_pte(pml4, va);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    *out_pa = *pte & PTE_ADDR_MASK;
    return 0;
}

/*
 * ★ 修复 10：解析库导出符号表。
 *
 * 人工必须审查：
 *   - 遍历 .dynsym，跳过 SHN_UNDEF（未定义）和 st_value==0 的符号。
 *   - 从 .dynstr 读取符号名，复制到 out->exports[]。
 *   - 18D 阶段 pe_to_obr.py 不生成 .dynsym，因此 export_count 通常为 0；
 *     find_symbol_in_loaded 会 fallback 到同库本地定义。
 */
static void parse_lib_exports(struct task_t *t, struct dyn_lib *out)
{
    if (!out) return;
    out->export_count = 0;

    if (!out->dynsym_va || !out->dynstr_va || !out->strtab_size) return;

    uint64_t *pml4 = task_user_pml4_ptr(t);
    if (!pml4) return;

    /* 估算符号数：strtab_size / 24 是保守估计，实际以遍历为准 */
    uint32_t max_syms = (uint32_t)(out->strtab_size / 24u);
    if (max_syms == 0) max_syms = 16;
    if (max_syms > 256) max_syms = 256;

    for (uint32_t i = 0;
         i < max_syms && out->export_count < DYN_MAX_EXPORTS;
         ++i) {
        uint64_t sym_va = out->dynsym_va + (uint64_t)i * sizeof(struct obr_sym);

        uint64_t spa = 0;
        if (user_va_to_phys(pml4, sym_va, &spa) != 0) break;
        uint64_t sw = sym_va & 0xFFF;
        if (sw + sizeof(struct obr_sym) > PAGE_SIZE) break;
        const struct obr_sym *sym =
            (const struct obr_sym *)(DIRECTMAP_BASE + spa + sw);

        /* 跳过未定义或未赋值的符号 */
        if (sym->st_shndx == 0) continue;
        if (sym->st_value == 0) continue;

        /* 读符号名 */
        uint64_t nva = out->dynstr_va + sym->st_name;
        uint64_t npa = 0;
        if (user_va_to_phys(pml4, nva, &npa) != 0) continue;
        uint64_t nw = nva & 0xFFF;
        if (nw + 1 > PAGE_SIZE) continue;
        const char *np = (const char *)(DIRECTMAP_BASE + npa + nw);

        int k = 0;
        while (np[k] && k < 63) {
            out->exports[out->export_count].name[k] = np[k];
            ++k;
        }
        out->exports[out->export_count].name[k] = '\0';
        out->exports[out->export_count].addr =
            out->base_vaddr + sym->st_value;
        out->export_count++;
    }

    if (out->export_count > 0) {
        serial_printf("[DYN] '%s': parsed %u exports\n",
                      out->name, (unsigned)out->export_count);
    }
}

static int map_lib_obr(struct task_t *t, const struct obr_header *h,
                       const uint8_t *img, uint64_t size,
                       uint64_t base_va, struct dyn_lib *out)
{
    if (h->magic != OBR_MAGIC) return -1;
    if (h->arch != OBR_ARCH_X64) return -1;
    if (h->ph_offset + (uint64_t)h->ph_count * sizeof(struct obr_phdr) > size)
        return -1;

    struct user_ctx *c = user_get_ctx(t);
    if (!c || !c->pml4) {
        serial_printf("[DYN] map_lib_obr: no user_ctx pml4\n");
        return -1;
    }
    uint64_t *pml4 = c->pml4;

    for (uint16_t i = 0; i < h->ph_count; ++i) {
        const struct obr_phdr *ph = (const struct obr_phdr *)
            (img + h->ph_offset + (uint64_t)i * sizeof(*ph));
        if (ph->type != OBR_PT_LOAD) continue;
        if (ph->offset + ph->filesz > size) return -1;

        uint64_t dst_va = base_va + ph->vaddr;
        if (!user_range_ok(dst_va, ph->memsz)) return -1;
        uint64_t npages = (ph->memsz + PAGE_SIZE - 1) / PAGE_SIZE;
        if (npages == 0) continue;

        for (uint64_t p = 0; p < npages; ++p) {
            struct page *pg = pmm_alloc_pages(0);
            if (!pg) return -1;
            uint64_t pa = page_to_phys(pg);
            uint64_t va = dst_va + p * PAGE_SIZE;

            uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            if (!(ph->flags & OBR_PF_X)) flags |= PTE_NX;
            if (vmm_map_page(pml4, va, pa, flags) != 0) {
                pmm_free_pages(pg, 0);
                return -1;
            }
        }

        for (uint64_t p = 0; p < npages; ++p) {
            uint64_t va = dst_va + p * PAGE_SIZE;
            uint64_t *pte = vmm_get_pte(pml4, va);
            if (!pte || !(*pte & PTE_PRESENT)) continue;
            uint64_t pa = *pte & PTE_ADDR_MASK;
            uint8_t *dst = (uint8_t *)(DIRECTMAP_BASE + pa);
            uint64_t begin = p * PAGE_SIZE;
            uint64_t end = begin + PAGE_SIZE;
            if (end > ph->filesz) end = ph->filesz;
            if (begin >= ph->filesz) {
                for (uint64_t k = 0; k < PAGE_SIZE; ++k) dst[k] = 0;
            } else {
                const uint8_t *src = img + ph->offset;
                for (uint64_t k = begin; k < end; ++k)
                    dst[k - begin] = src[k];
                for (uint64_t k = end - begin; k < PAGE_SIZE; ++k)
                    dst[k] = 0;
            }
        }
    }

    out->base_vaddr = base_va;
    out->size = 0;
    out->dynamic_va = 0;
    out->dynsym_va = 0;
    out->dynstr_va = 0;
    out->rela_va = 0;
    out->rela_size = 0;
    out->jmprel_va = 0;
    out->jmprel_size = 0;
    out->strtab_size = 0;
    out->symtab_entsz = 0;
    out->rela_entsz = 0;
    out->jmprel_entsz = 0;
    out->flags = 0;
    out->export_count = 0;
    return 0;
}

static int lib_search_and_map(struct task_t *t, const char *name,
                              struct dyn_lib *out)
{
    if (!t || !name || !out) return -1;

    char path[256];
    const char *prefixes[] = { "/system/lib/", "/usr/lib/", 0 };
    struct vfs_file *f = 0;
    int rc = -1;

    for (int i = 0; prefixes[i]; ++i) {
        int pl = 0;
        while (prefixes[i][pl]) ++pl;
        int nl = 0;
        while (name[nl] && (pl + nl + 1) < 255) ++nl;
        if (pl + nl + 1 >= 255) continue;
        for (int k = 0; k < pl; ++k) path[k] = prefixes[i][k];
        for (int k = 0; k < nl; ++k) path[pl + k] = name[k];
        path[pl + nl] = '\0';

        serial_printf("[DYN] trying path: %s\n", path);
        rc = vfs_open(path, VFS_O_RDONLY, &f);
        if (rc == 0 && f) break;
    }
    if (rc != 0 || !f) {
        serial_printf("[DYN] library '%s' not found\n", name);
        return -1;
    }

    uint64_t fsize = f->f_inode ? f->f_inode->size : 0;
    if (fsize == 0) {
        serial_printf("[DYN] library '%s' size=0\n", name);
        vfs_close(f);
        return -1;
    }
    if (fsize > 256 * 1024) {
        serial_printf("[DYN] library '%s' too large (%llu)\n",
                      name, (unsigned long long)fsize);
        vfs_close(f);
        return -1;
    }

    uint64_t pages = (fsize + PAGE_SIZE - 1) / PAGE_SIZE;
    int order = 0;
    while (order < MAX_ORDER && ((uint64_t)1 << order) < pages) order++;
    if (order >= MAX_ORDER) {
        vfs_close(f);
        return -1;
    }

    struct page *pg = pmm_alloc_pages(order);
    if (!pg) {
        serial_printf("[DYN] pmm_alloc_pages(order=%d) failed\n", order);
        vfs_close(f);
        return -1;
    }
    uint8_t *buf = (uint8_t *)(uintptr_t)
        (DIRECTMAP_BASE + page_to_phys(pg));

    int64_t total = vfs_read(f, buf, fsize);
    vfs_close(f);
    if (total <= 0 || (uint64_t)total != fsize) {
        serial_printf("[DYN] read '%s' failed total=%lld\n",
                      name, (long long)total);
        pmm_free_pages(pg, order);
        return -1;
    }

    uint64_t base_va = 0x0000700000000000ULL +
                       ((uint64_t)(uint8_t)name[0] << 21);

    const struct obr_header *h = (const struct obr_header *)buf;
    int mrc = map_lib_obr(t, h, buf, fsize, base_va, out);
    pmm_free_pages(pg, order);
    if (mrc != 0) {
        serial_printf("[DYN] map_lib_obr failed for '%s'\n", name);
        return -1;
    }

    int i = 0;
    while (name[i] && i < DYN_NAME_MAX - 1) {
        out->name[i] = name[i]; ++i;
    }
    out->name[i] = '\0';

    /* ★ 修复 10：解析导出符号表 */
    parse_lib_exports(t, out);

    /* ★ 修复 10：注册到全局库集合 */
    dyn_register_lib(out);

    serial_printf("[DYN] mapped lib '%s' at 0x%llx exports=%u\n",
                  name, (unsigned long long)base_va,
                  (unsigned)out->export_count);
    return 0;
}

/* ★ 修复 10：find_symbol_in_loaded 转发到 dyn_find_symbol */
static int find_symbol_in_loaded(const char *name, uint64_t *out_addr)
{
    return dyn_find_symbol(name, out_addr);
}

/*
 * ★★★ 修复 A v2：libs[] 从栈移到文件级 static ★★★
 *
 * 人工必须审查：
 *   - sizeof(struct dyn_lib) ≈ 4792 字节（含 exports[64]）。
 *   - DYN_MAX_LIBS = 8 → libs[8] ≈ 37.4 KB。
 *   - 内核线程栈只有 THREAD_KSTACK_SIZE = 16 KB。
 *   - 在栈上分配 libs[8] 会溢出内核栈，覆写 .bss 或相邻内核数据结构。
 *   - 症状：
 *       1) 修改前（无显式清零）：-O2 下编译器部分优化栈写入，
 *          溢出无害，但 deps_blob 被 pmm 复用后 name 乱码；
 *       2) 修改后（显式清零）：强制写入全部 38336 字节，栈溢出实际
 *          发生，破坏 vfs_open 路径依赖的内核数据；累积效应导致
 *          后续 pipe_test.obr lookup ENOENT。
 *   - 修复：改为文件级 static，生命周期覆盖 elf_load_deps 整个调用；
 *     elf_load_deps 是单线程顺序调用（elf loader 路径唯一），
 *     无需加锁。
 *   - 每次调用前清零，避免上次调用的残留数据污染日志。
 *
 * ★★★ 命名说明（本轮修复）★★★
 *   文件顶部已有一个 g_dyn_libs[DYN_MAX_LIBS]（类型 struct dyn_lib *[8]），
 *   它是"已加载库指针数组"，供 dyn_find_symbol() 使用。
 *   本 storage 数组是"存放实际库结构体"的地方，两者用途不同，
 *   必须用不同名字：本数组命名为 g_load_deps_libs_storage。
 */
static struct dyn_lib g_load_deps_libs_storage[DYN_MAX_LIBS];

int elf_load_deps(uint64_t *exe_pml4,
                  const char *deps_blob,
                  uint32_t deps_count)
{
    (void)exe_pml4;
    if (!deps_blob || deps_count == 0) return 0;
    if (deps_count > DYN_MAX_LIBS) return -1;

    struct task_t *t = task_from_thread(sched_current());
    if (!t) return -1;

    /* ★ 使用文件级 static 数组，避免 37 KB 栈数组溢出 16 KB 内核栈 */
    struct dyn_lib *libs = g_load_deps_libs_storage;

    /* 清零：避免上次调用的残留数据污染 name 字段 */
    {
        uint8_t *p = (uint8_t *)libs;
        for (unsigned i = 0; i < sizeof(g_load_deps_libs_storage); ++i)
            p[i] = 0;
    }

    uint32_t idx = 0;
    const char *p = deps_blob;
    for (uint32_t i = 0; i < deps_count; ++i) {
        if (lib_search_and_map(t, p, &libs[idx]) == 0) idx++;
        int n = 0;
        while (p[n]) ++n;
        p += n + 1;
    }
    serial_printf("[DYN] loaded %u/%u dependencies\n",
                  (unsigned)idx, (unsigned)deps_count);
    return 0;
}

int elf_apply_relocations(struct dyn_lib *lib)
{
    if (!lib) return -1;

    struct task_t *t = task_from_thread(sched_current());
    if (!t) return -1;
    struct user_ctx *c = user_get_ctx(t);
    if (!c || !c->pml4) return -1;

    if (!lib->rela_va && !lib->jmprel_va) {
        serial_printf("[DYN] '%s': no relocations\n", lib->name);
        return 0;
    }

    serial_printf("[DYN] applying relocations for '%s' "
                  "(rela=0x%llx+0x%llx, jmprel=0x%llx+0x%llx)\n",
                  lib->name,
                  (unsigned long long)lib->rela_va,
                  (unsigned long long)lib->rela_size,
                  (unsigned long long)lib->jmprel_va,
                  (unsigned long long)lib->jmprel_size);

    struct { uint64_t va, size; } regions[2] = {
        { lib->rela_va,   lib->rela_size   },
        { lib->jmprel_va, lib->jmprel_size },
    };

    for (int r = 0; r < 2; ++r) {
        if (regions[r].va == 0 || regions[r].size == 0) continue;

        uint64_t off = regions[r].va;
        uint64_t end = regions[r].va + regions[r].size;

        while (off + sizeof(struct obr_rela) <= end) {
            struct obr_rela rela;

            uint64_t pa = 0;
            if (user_va_to_phys(c->pml4, off, &pa) != 0) break;
            uint64_t within = off & 0xFFFULL;
            if (within + sizeof(rela) > PAGE_SIZE) break;
            const uint8_t *src =
                (const uint8_t *)(DIRECTMAP_BASE + pa + within);
            for (size_t k = 0; k < sizeof(rela); ++k)
                ((uint8_t *)&rela)[k] = src[k];

            uint32_t type = (uint32_t)(rela.r_info & 0xFFFFFFFFULL);
            uint64_t target_va = lib->base_vaddr + rela.r_offset;

            uint64_t tpa = 0;
            if (user_va_to_phys(c->pml4, target_va, &tpa) != 0) {
                serial_printf("[DYN] rela: target va 0x%llx unmapped\n",
                              (unsigned long long)target_va);
                return -1;
            }
            uint64_t twithin = target_va & 0xFFFULL;
            if (twithin + 8 > PAGE_SIZE) return -1;
            uint64_t *slot = (uint64_t *)(DIRECTMAP_BASE + tpa + twithin);

            switch (type) {
            case R_X86_64_RELATIVE:
                *slot = lib->base_vaddr + (uint64_t)rela.r_addend;
                break;

            case R_X86_64_64:
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: {
                uint32_t sym_idx = (uint32_t)(rela.r_info >> 32);
                if (lib->dynsym_va == 0) return -1;
                uint64_t sym_va = lib->dynsym_va +
                                  (uint64_t)sym_idx * sizeof(struct obr_sym);
                uint64_t spa = 0;
                if (user_va_to_phys(c->pml4, sym_va, &spa) != 0) return -1;
                uint64_t swithin = sym_va & 0xFFFULL;
                if (swithin + sizeof(struct obr_sym) > PAGE_SIZE) return -1;
                const struct obr_sym *sym =
                    (const struct obr_sym *)(DIRECTMAP_BASE + spa + swithin);

                /* ★ 修复 10：跨库符号解析 */
                if (sym->st_shndx == 0 /* SHN_UNDEF */) {
                    char sym_name[128];
                    if (lib->dynstr_va) {
                        uint64_t nva = lib->dynstr_va + sym->st_name;
                        uint64_t npa = 0;
                        if (user_va_to_phys(c->pml4, nva, &npa) == 0) {
                            const char *np =
                                (const char *)(DIRECTMAP_BASE + npa);
                            int len = 0;
                            while (np[len] && len < 127) {
                                sym_name[len] = np[len];
                                ++len;
                            }
                            sym_name[len] = '\0';
                        } else {
                            sym_name[0] = '\0';
                        }
                    } else {
                        sym_name[0] = '\0';
                    }

                    uint64_t addr = 0;
                    if (find_symbol_in_loaded(sym_name, &addr) != 0) {
                        addr = lib->base_vaddr + sym->st_value;
                        serial_printf("[DYN] unresolved symbol '%s' "
                                      "(using local def)\n", sym_name);
                    } else {
                        serial_printf("[DYN] cross-lib symbol '%s' -> "
                                      "0x%llx\n",
                                      sym_name, (unsigned long long)addr);
                    }
                    *slot = addr + (uint64_t)rela.r_addend;
                } else {
                    *slot = lib->base_vaddr + sym->st_value +
                            (uint64_t)rela.r_addend;
                }
                break;
            }
            default:
                serial_printf("[DYN] unsupported reloc type=%u\n",
                              (unsigned)type);
                return -1;
            }
            off += sizeof(struct obr_rela);
        }
    }

    serial_printf("[DYN] relocations applied for '%s'\n", lib->name);
    return 0;
}

/* ============================================================
 * ★ 第 19 步：Linux ELF64 加载器
 *
 * 人工必须审查（本段核心安全点）：
 *   1) elf64_detect 只做形态判定，不解析段，不做任何副作用。
 *   2) elf64_load_into_task 每个 PT_LOAD 段都要做完整的边界检查，
 *      所有地址运算必须是 uint64_t，避免 32 位截断。
 *   3) PT_INTERP 存在时显式拒绝，绝不静默忽略。
 *   4) 每个新分配的页必须先清零再映射内容，避免泄漏内核内存内容。
 *   5) 越界、重叠、溢出都返回负错误码；调用方负责 user_teardown。
 * ============================================================ */

int elf64_detect(const void *buf, uint64_t size)
{
    if (!buf) return 0;
    if (size < sizeof(struct elf64_ehdr)) return 0;

    const uint8_t *p = (const uint8_t *)buf;
    if (p[0] != 0x7F || p[1] != 'E' || p[2] != 'L' || p[3] != 'F') return 0;
    if (p[ELF_EI_CLASS] != ELF_ELFCLASS64) return -1;
    if (p[ELF_EI_DATA]  != ELF_ELFDATA2LSB) return -1;

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)buf;
    if (eh->e_machine != ELF_EM_X86_64) return -1;
    if (eh->e_type != ELF_ET_EXEC && eh->e_type != ELF_ET_DYN) return -1;
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) return -1;
    return 1;
}

/*
 * 检测给定 VA 是否已经被映射为用户页。
 * 返回 1=已映射，0=未映射，-1=存在但非用户页。
 */
static int elf64_va_mapped(uint64_t *pml4, uint64_t va, int *out_present)
{
    *out_present = 0;
    uint64_t *pte = vmm_get_pte(pml4, va);
    if (!pte) return 0;
    if (!(*pte & PTE_PRESENT)) return 0;
    if (!(*pte & PTE_USER)) return -1;
    *out_present = 1;
    return 1;
}

int elf64_load_into_task(struct task_t *t,
                         const uint8_t *img, uint64_t size,
                         uint64_t *entry_va,
                         uint64_t *phdr_va, uint64_t *phnum)
{
    if (!t || !img || !entry_va) return OB_EINVAL;

    int d = elf64_detect(img, size);
    if (d == 0) return OB_EINVAL;
    if (d < 0)  return OB_EIO;

    struct user_ctx *c = user_get_ctx(t);
    if (!c || !c->pml4) {
        serial_printf("[ELF64] no user_ctx/pml4 for pid=%llu\n",
                      (unsigned long long)t->pid);
        return OB_EPERM;
    }

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;

    /* 程序头表边界检查（防溢出） */
    if (eh->e_phnum == 0) return OB_EIO;
    uint64_t ph_table_bytes = (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
    if (eh->e_phoff + ph_table_bytes < eh->e_phoff) return OB_EIO;
    if (eh->e_phoff + ph_table_bytes > size)         return OB_EIO;

    uint64_t phdr_va_out = 0;
    int      phdr_seen   = 0;
    uint32_t phnum_out   = eh->e_phnum;

    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const struct elf64_phdr *ph = (const struct elf64_phdr *)
            (img + eh->e_phoff + (uint64_t)i * sizeof(struct elf64_phdr));

        if (ph->p_type == ELF_PT_INTERP) {
            serial_printf("[ELF64] WARN: PT_INTERP present, "
                          "dynamic interpreter not supported in step 19\n");
            return OB_ENOSYS;
        }
        if (ph->p_type == ELF_PT_PHDR) {
            phdr_va_out = ph->p_vaddr;
            phdr_seen   = 1;
            continue;
        }
        if (ph->p_type == ELF_PT_DYNAMIC) {
            serial_printf("[ELF64] INFO: PT_DYNAMIC present, "
                          "ignored in step 19 (static-only)\n");
            continue;
        }
        if (ph->p_type != ELF_PT_LOAD) continue;

        /* ---- 段边界检查 ---- */
        if (ph->p_offset + ph->p_filesz < ph->p_offset) return OB_EIO;
        if (ph->p_offset + ph->p_filesz > size)         return OB_EIO;
        if (ph->p_filesz > ph->p_memsz)                 return OB_EIO;

        uint64_t seg_start = ph->p_vaddr;
        uint64_t seg_end   = ph->p_vaddr + ph->p_memsz;
        if (seg_end < seg_start) return OB_EINVAL;
        if (seg_start < USER_SPACE_START) return OB_EINVAL;
        if (seg_end - 1 > USER_SPACE_END) return OB_EINVAL;
        if (!user_range_ok(seg_start, ph->p_memsz)) return OB_EINVAL;

        /* 计算页对齐范围 */
        uint64_t page_start = seg_start & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t page_end   = (seg_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (page_end < page_start) return OB_EINVAL;

        for (uint64_t va = page_start; va < page_end; va += PAGE_SIZE) {
    /* ★ 第 19 步防御：检测目标 VA 是否已落在 2MB 大页中。
     *
     * 人工必须审查：
     *   - 内核 PML4[0] 的低恒等映射（0..4GB）使用 2MB 大页；
     *   - 若 ELF 的 PT_LOAD 段落在此范围内（例如 ld.lld 默认基址
     *     0x200000），vmm_map_page 会因 PTE_HUGE 冲突返回 -2；
     *   - 这里显式打印诊断信息，指导工具链使用高 VA（例如
     *     -Wl,-Ttext-segment=0x8000000000）。
     *   - 本检查只读、无副作用；不尝试拆大页（拆页会与内核恒等
     *     映射语义冲突）。 */
    {
        uint64_t i4 = (va >> 39) & 0x1FF;
        if (i4 == 0) {
            uint64_t pdpt_pa = c->pml4[i4] & PTE_ADDR_MASK;
            if ((c->pml4[i4] & PTE_PRESENT) && pdpt_pa) {
                uint64_t *pdpt = (uint64_t *)(DIRECTMAP_BASE + pdpt_pa);
                uint64_t i3 = (va >> 30) & 0x1FF;
                if ((pdpt[i3] & PTE_PRESENT) && !(pdpt[i3] & PTE_HUGE)) {
                    uint64_t pd_pa = pdpt[i3] & PTE_ADDR_MASK;
                    uint64_t *pd = (uint64_t *)(DIRECTMAP_BASE + pd_pa);
                    uint64_t i2 = (va >> 21) & 0x1FF;
                    if ((pd[i2] & PTE_PRESENT) && (pd[i2] & PTE_HUGE)) {
                        serial_printf("[ELF64] FATAL: PT_LOAD VA=0x%llx "
                                      "conflicts with 2MB HUGE identity page "
                                      "(pd[%llu]=0x%llx).\n",
                                      (unsigned long long)va,
                                      (unsigned long long)i2,
                                      (unsigned long long)pd[i2]);
                        serial_printf("[ELF64]   HINT: rebuild the Linux ELF "
                                      "with a high VA, e.g. "
                                      "clang -Wl,-Ttext-segment=0x8000000000\n");
                        return OB_EINVAL;
                    }
                }
            }
        }
    }

    int present = 0;
    int rc0 = elf64_va_mapped(c->pml4, va, &present);
            if (rc0 < 0) {
                serial_printf("[ELF64] WARN: va=0x%llx exists but not user\n",
                              (unsigned long long)va);
                return OB_EIO;
            }
            if (present) continue;   /* 与其它段重叠，复用已有页 */

            struct page *pg = pmm_alloc_pages(0);
            if (!pg) return OB_ENOMEM;
            uint64_t pa = page_to_phys(pg);

            uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            /* 若段不可执行，加 NX；但可执行段不加。对 PT_LOAD 段
             * 而言，p_flags 描述的是“最终”权限；我们直接采用。 */
            if (!(ph->p_flags & ELF_PF_X)) flags |= PTE_NX;

            int rc = vmm_map_page(c->pml4, va, pa, flags);
            if (rc != 0) {
                pmm_free_pages(pg, 0);
                serial_printf("[ELF64] vmm_map_page va=0x%llx rc=%d\n",
                              (unsigned long long)va, rc);
                return OB_EIO;
            }

            uint8_t *dst = (uint8_t *)(DIRECTMAP_BASE + pa);
            for (uint64_t k = 0; k < PAGE_SIZE; ++k) dst[k] = 0;
        }

        /* ---- 拷贝文件内容（仅 p_filesz 字节；其余为零） ---- */
        uint64_t copy_len = ph->p_filesz;
        for (uint64_t k = 0; k < copy_len; ) {
            uint64_t va  = seg_start + k;
            uint64_t pgo = va & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t off = va - pgo;
            uint64_t take = PAGE_SIZE - off;
            if (take > copy_len - k) take = copy_len - k;

            uint64_t *pte = vmm_get_pte(c->pml4, pgo);
            if (!pte || !(*pte & PTE_PRESENT)) {
                serial_printf("[ELF64] FATAL: pte vanished at 0x%llx\n",
                              (unsigned long long)pgo);
                return OB_EIO;
            }
            uint64_t pa = *pte & PTE_ADDR_MASK;
            uint8_t *dst = (uint8_t *)(DIRECTMAP_BASE + pa) + off;
            const uint8_t *src = img + ph->p_offset + k;
            for (uint64_t j = 0; j < take; ++j) dst[j] = src[j];
            k += take;
        }
    }

    *entry_va = eh->e_entry;
    if (phdr_va) *phdr_va = phdr_va_out;
    if (phnum)   *phnum   = (uint16_t)phnum_out;

    serial_printf("[ELF64] loaded pid=%llu entry=0x%llx phnum=%u "
                  "phdr_va=0x%llx\n",
                  (unsigned long long)t->pid,
                  (unsigned long long)eh->e_entry,
                  (unsigned)eh->e_phnum,
                  (unsigned long long)phdr_va_out);
    (void)phdr_seen;
    return 0;
}

/* ============================================================
 * Linux 初始用户栈构建
 *
 * 内存布局（从低到高）：
 *     [argc]  [argv[0..argc-1]] [NULL]
 *             [envp[0..envc-1]] [NULL]
 *             [auxv pair 0..N-1] [AT_NULL/0]
 *             <字符串区>
 *   <-- 低地址                                     高地址 -->
 *
 * 人工必须审查：
 *   1) 从高地址向低地址逐项递减 rsp；每次写入后 16 字节对齐。
 *   2) AT_RANDOM 指向 16 字节随机数，由 rng_bytes 生成。
 *   3) 任一 user_stack_write 失败，返回 0。
 *   4) 最终 RSP 指向 argc（int），且 RSP % 16 == 0。
 * ============================================================ */

/* AT_* 常量（与 Linux x86_64 一致） */
#define LX_AT_NULL     0
#define LX_AT_PHDR     3
#define LX_AT_PHENT    4
#define LX_AT_PHNUM    5
#define LX_AT_PAGESZ   6
#define LX_AT_ENTRY    9
#define LX_AT_UID      11
#define LX_AT_EUID     12
#define LX_AT_GID      13
#define LX_AT_EGID     14
#define LX_AT_CLKTCK   17
#define LX_AT_RANDOM   25
#define LX_AT_EXECFN   31

uint64_t linux_build_stack(struct task_t *t,
                           int argc, const char **argv,
                           int envc, const char **envp,
                           uint64_t entry_va,
                           uint64_t phdr_va, uint16_t phnum,
                           const char *execfn)
{
    struct user_ctx *c = user_get_ctx(t);
    if (!c) return 0;

    if (argc < 0) argc = 0;
    if (argc > 32) argc = 32;
    if (envc < 0) envc = 0;
    if (envc > 32) envc = 32;

    uint64_t sp = c->stack_top;

    /* ---- 1) 16 字节随机数（AT_RANDOM） ---- */
    sp -= 16;
    sp &= ~0xFULL;
    uint64_t rand_va = sp;
    {
        uint8_t rnd[16];
        if (rng_bytes(rnd, 16) != 0) {
            /* RNG 失败时降级为固定模式；不允许阻塞 */
            for (int i = 0; i < 16; ++i) rnd[i] = (uint8_t)(i * 7 + 3);
        }
        if (user_stack_write(c, rand_va, rnd, 16) != 0) return 0;
    }

    /* ---- 2) execfn 字符串 ---- */
    uint64_t execfn_va = 0;
    if (execfn && execfn[0]) {
        uint64_t len = 0;
        while (execfn[len] && len < 255) ++len;
        ++len;   /* NUL */
        sp -= len;
        sp &= ~0xFULL;
        if (user_stack_write(c, sp, execfn, len) != 0) return 0;
        execfn_va = sp;
    }

    /* ---- 3) envp 字符串（逆序） ---- */
    uint64_t env_ptrs[32];
    for (int i = 0; i < envc; ++i) env_ptrs[i] = 0;
    for (int i = envc - 1; i >= 0; --i) {
        const char *s = (envp && envp[i]) ? envp[i] : "";
        uint64_t len = 0;
        while (s[len] && len < 511) ++len;
        ++len;
        sp -= len;
        sp &= ~0xFULL;
        if (user_stack_write(c, sp, s, len) != 0) return 0;
        env_ptrs[i] = sp;
    }

    /* ---- 4) argv 字符串（逆序） ---- */
    uint64_t arg_ptrs[32];
    for (int i = 0; i < argc; ++i) arg_ptrs[i] = 0;
    for (int i = argc - 1; i >= 0; --i) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        uint64_t len = 0;
        while (s[len] && len < 511) ++len;
        ++len;
        sp -= len;
        sp &= ~0xFULL;
        if (user_stack_write(c, sp, s, len) != 0) return 0;
        arg_ptrs[i] = sp;
    }

    /* ---- 5) auxv 表（逆序写入） ---- */
    struct { uint64_t type; uint64_t value; } auxv[] = {
        { LX_AT_PAGESZ, 4096ULL },
        { LX_AT_ENTRY,  entry_va },
        { LX_AT_PHDR,   phdr_va },
        { LX_AT_PHNUM,  (uint64_t)phnum },
        { LX_AT_PHENT,  (uint64_t)sizeof(struct elf64_phdr) },
        { LX_AT_UID,    0ULL },
        { LX_AT_EUID,   0ULL },
        { LX_AT_GID,    0ULL },
        { LX_AT_EGID,   0ULL },
        { LX_AT_CLKTCK, 100ULL },
        { LX_AT_RANDOM, rand_va },
        { LX_AT_EXECFN, execfn_va },
        { LX_AT_NULL,   0ULL },
    };
    const int auxv_n = (int)(sizeof(auxv) / sizeof(auxv[0]));

    for (int i = auxv_n - 1; i >= 0; --i) {
        sp -= 16;
        sp &= ~0xFULL;
        uint64_t pair[2] = { auxv[i].type, auxv[i].value };
        if (user_stack_write(c, sp, pair, 16) != 0) return 0;
    }

    /* ---- 6) envp 结束 NULL ---- */
    sp -= 8;
    sp &= ~0xFULL;
    { uint64_t zero = 0; if (user_stack_write(c, sp, &zero, 8) != 0) return 0; }

    /* ---- 7) envp 指针数组（逆序） ---- */
    for (int i = envc - 1; i >= 0; --i) {
        sp -= 8;
        sp &= ~0xFULL;
        if (user_stack_write(c, sp, &env_ptrs[i], 8) != 0) return 0;
    }

    /* ---- 8) argv 结束 NULL ---- */
    sp -= 8;
    sp &= ~0xFULL;
    { uint64_t zero = 0; if (user_stack_write(c, sp, &zero, 8) != 0) return 0; }

    /* ---- 9) argv 指针数组（逆序） ---- */
    for (int i = argc - 1; i >= 0; --i) {
        sp -= 8;
        sp &= ~0xFULL;
        if (user_stack_write(c, sp, &arg_ptrs[i], 8) != 0) return 0;
    }

    /* ---- 10) argc（最终 RSP） ---- */
    sp -= 8;
    sp &= ~0xFULL;
    { uint64_t a64 = (uint64_t)argc;
      if (user_stack_write(c, sp, &a64, 8) != 0) return 0; }

    /* 最终 RSP 必须 16 字节对齐（Linux x86_64 ABI 要求）
     * 已通过每次 sp &= ~0xF 保证。 */
    serial_printf("[ELF64] linux stack: argc=%d envc=%d auxv=%d "
                  "rand=0x%llx execfn=0x%llx rsp=0x%llx\n",
                  argc, envc, auxv_n,
                  (unsigned long long)rand_va,
                  (unsigned long long)execfn_va,
                  (unsigned long long)sp);
    return sp;
}
/*===OmniBridgeOs/kernel/arch/x64/user/elf_loader.c 结束===*/