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

void elf_loader_init(void);

int elf_load_deps(uint64_t *exe_pml4,
                  const char *deps_blob,
                  uint32_t deps_count);

int elf_apply_relocations(struct dyn_lib *lib);

/* ★ 修复 10：注册已加载的库（供后续跨库符号查找）。
 *
 * 人工必须审查：
 *   - lib 指针必须指向调用者持有的稳定存储（不会在后续调用中失效）。
 *   - 内部维护固定大小的全局库集合，容量 DYN_MAX_LIBS。 */
void dyn_register_lib(struct dyn_lib *lib);

/* ★ 修复 10：在已加载库集合中查找符号。
 *   成功返回 0 并写入 *out_addr；未找到返回 -1。 */
int dyn_find_symbol(const char *name, uint64_t *out_addr);

#endif /* OMNIBRIDGE_USER_ELF_LOADER_H */
/*===OmniBridgeOs/kernel/arch/x64/user/elf_loader.h 结束===*/