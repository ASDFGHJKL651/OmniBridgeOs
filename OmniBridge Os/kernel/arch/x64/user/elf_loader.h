/*===OmniBridgeOs/kernel/arch/x64/user/elf_loader.h===*/
#ifndef OMNIBRIDGE_USER_ELF_LOADER_H
#define OMNIBRIDGE_USER_ELF_LOADER_H

#include <stdint.h>
#include "task.h"

#define DYN_MAX_LIBS     8
#define DYN_MAX_SYMS     256
#define DYN_NAME_MAX     64

/* ★ 修复 10：每个库的导出符号上限 */
#define DYN_MAX_EXPORTS  64

struct dyn_lib {
    char     name[DYN_NAME_MAX];
    uint64_t base_vaddr;
    uint64_t size;
    uint64_t dynamic_va;
    uint64_t dynsym_va;
    uint64_t dynstr_va;
    uint64_t rela_va;
    uint64_t rela_size;
    uint64_t jmprel_va;
    uint64_t jmprel_size;
    uint64_t strtab_size;
    uint64_t symtab_entsz;
    uint64_t rela_entsz;
    uint64_t jmprel_entsz;
    uint64_t flags;

    /* ★ 修复 10：导出符号表（由 map_lib_obr 填充） */
    struct {
        char     name[64];
        uint64_t addr;
    } exports[DYN_MAX_EXPORTS];
    uint32_t export_count;
    uint32_t _pad_exports;
};

/* ============================================================
 * ★ 第 19 步：Linux ELF64 支持
 *
 * 人工必须审查：
 *   - ELF64 头与 program header 必须严格 packed（无填充）。
 *   - 常量与 System V x86_64 psABI 一致。
 *   - elf64_detect 返回值：1=ELF64 可执行；0=非 ELF；-1=是 ELF
 *     但类别/字节序/机器不符。
 * ============================================================ */

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;        /* ET_EXEC=2, ET_DYN=3 */
    uint16_t e_machine;     /* EM_X86_64=62 */
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define ELF_ET_EXEC      2
#define ELF_ET_DYN       3
#define ELF_EM_X86_64    62
#define ELF_PT_LOAD      1
#define ELF_PT_DYNAMIC   2
#define ELF_PT_INTERP    3
#define ELF_PT_PHDR      6
#define ELF_PF_X         1
#define ELF_PF_W         2
#define ELF_PF_R         4

#define ELF_EI_CLASS     4
#define ELF_EI_DATA      5
#define ELF_ELFCLASS64   2
#define ELF_ELFDATA2LSB  1

/* 检测 buf 是否为可执行 Linux ELF64。
 *  返回  1 : 是 ELF64 ET_EXEC / ET_DYN / EM_X86_64
 *  返回  0 : 不是 ELF 文件
 *  返回 -1 : 是 ELF，但类别/字节序/机器/类型不兼容 */
int elf64_detect(const void *buf, uint64_t size);

/* 加载 Linux ELF64 到 task t 的用户地址空间。
 * 人工必须审查：
 *   - 所有地址计算必须用 uint64_t，防止整数溢出；
 *   - p_offset + p_filesz 必须 <= size；
 *   - p_filesz <= p_memsz；
 *   - p_vaddr..p_vaddr+p_memsz 必须落在 [USER_SPACE_START, USER_SPACE_END]；
 *   - PT_INTERP 出现时返回 -ENOSYS（不静默忽略）；
 *   - PT_DYNAMIC 记录到 phdr_va 之外的字段，本步仅打印 WARN；
 *   - 失败时必须回滚已映射的页（本函数不负责回收用户内存；由调用方
 *     user_teardown 清理 PML4）。 */
int elf64_load_into_task(struct task_t *t,
                         const uint8_t *img, uint64_t size,
                         uint64_t *entry_va,
                         uint64_t *phdr_va, uint64_t *phnum);

/*
 * 为 Linux ELF 进程构建初始用户栈。
 *
 * 参数：
 *   t          —— task_t（必须已 user_setup 建立用户栈）
 *   argc       —— 参数个数（0..64）
 *   argv       —— 参数字符串数组（内核态；可为 NULL）
 *   envc       —— 环境变量个数（0..64）
 *   envp       —— 环境字符串数组（内核态；可为 NULL）
 *   entry_va   —— ELF 入口点 VA（用于 AT_ENTRY）
 *   phdr_va    —— PT_PHDR 的 VA（若无则填 0）
 *   phnum      —— PT 条目数（用于 AT_PHNUM）
 *   execfn     —— 可执行文件路径（用于 AT_EXECFN；可为 NULL）
 *
 * 返回初始 RSP（用户栈 VA），16 字节对齐；失败返回 0。
 *
 * 人工必须审查：
 *   - 栈顶（argc 所在地址）必须 16 字节对齐（Linux x86_64 ABI）；
 *   - 字符串先写入高地址，指针数组后写入；
 *   - AT_RANDOM 指向 16 字节随机数（由 rng_bytes 生成）；
 *   - 任何 user_stack_write 失败必须立即返回 0。
 */
uint64_t linux_build_stack(struct task_t *t,
                           int argc, const char **argv,
                           int envc, const char **envp,
                           uint64_t entry_va,
                           uint64_t phdr_va, uint16_t phnum,
                           const char *execfn);

/* ---------- 现有 18D 接口 ---------- */
void elf_loader_init(void);

int elf_load_deps(uint64_t *exe_pml4,
                  const char *deps_blob,
                  uint32_t deps_count);

int elf_apply_relocations(struct dyn_lib *lib);

void dyn_register_lib(struct dyn_lib *lib);
int  dyn_find_symbol(const char *name, uint64_t *out_addr);

#endif /* OMNIBRIDGE_USER_ELF_LOADER_H */
/*===OmniBridgeOs/kernel/arch/x64/user/elf_loader.h 结束===*/