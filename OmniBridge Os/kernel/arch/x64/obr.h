/*===OmniBridgeOs/kernel/arch/x64/obr.h===*/
/*
 * .obr 最小加载器（第 10 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 严格按 §13 二进制布局解析。
 *   - 本步只支持 PT_LOAD；PT_DYNAMIC / PT_INTERP 跳过并警告。
 *   - 不做重定位、不做签名验证。
 *   - 所有边界检查必须用 uint64_t 计算，防止整数溢出。
 *   - filesz > memsz 视为格式错误。
 *
 * ★★★ 第 18C 步关键修复（本次）★★★
 *   原实现存在结构体大小与构建工具不匹配的严重 bug：
 *     - C 侧：sizeof(struct obr_header) = 68, sizeof(struct obr_phdr) = 48
 *     - Python 侧（scripts/pe_to_obr.py）：
 *         OBR_HDR_SIZE   = 80
 *         OBR_PHDR_SIZE  = 56
 *   后果：C 代码按 48 字节步长遍历 phdr 数组，而 Python 按 56 字节
 *   步长写入 → 从第 2 个条目开始读取就越位 8 字节，读出的 type
 *   字段是垃圾（0 或 4096），导致 `if (ph->type != OBR_PT_LOAD)`
 *   把除第 1 段外的所有段跳过。
 *
 *   症状：用户程序一执行就 triple fault（访问 .rdata / .data 常量
 *   时 #PF，异常处理也用同一个未映射地址 → double fault）。
 *   串口在 "(CPL=3 next)" 后完全静默。
 *
 *   修复：给两个结构体添加显式填充，使 sizeof 分别等于 80 / 56，
 *   并在文件末尾用 _Static_assert 强制约束。任何一侧修改都必须
 *   同步更新 scripts/pe_to_obr.py。
 */
#ifndef OMNIBRIDGE_OBR_H
#define OMNIBRIDGE_OBR_H

#include <stdint.h>

/* 魔数："OBR " + version nibble（规范 §13） */
#define OBR_MAGIC        0x4F425220u
#define OBR_VERSION      0x0100u

#define OBR_ARCH_X64     0x01u
#define OBR_ARCH_X86     0x02u
#define OBR_ARCH_ARM64   0x03u

#define OBR_PT_LOAD      1u
#define OBR_PT_DYNAMIC   2u
#define OBR_PT_INTERP    3u

#define OBR_PF_X         0x1u
#define OBR_PF_W         0x2u
#define OBR_PF_R         0x4u
/* ★ 第 18C 步：PT_DYNAMIC 与 R_X86_64_* 重定位（最小子集） */
#define OBR_DT_NULL      0
#define OBR_DT_NEEDED    1
#define OBR_DT_STRTAB    5
#define OBR_DT_SYMTAB    6
#define OBR_DT_RELA      7
#define OBR_DT_RELASZ    8
#define OBR_DT_RELAENT   9
#define OBR_DT_STRSZ     10
#define OBR_DT_SYMENT    11
#define OBR_DT_JMPREL    23
#define OBR_DT_PLTRELSZ  2

struct obr_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed));

struct obr_sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed));

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

/*
 * ★ 与 scripts/pe_to_obr.py 的 OBR_HDR_SIZE 保持一致。
 *   原始 68 字节字段 + 12 字节显式填充 = 80。
 */
struct obr_header {
    uint32_t magic;               /* 0x4F425220 */
    uint16_t version;             /* 0x0100 */
    uint8_t  arch;                /* 0x01=x64 */
    uint8_t  min_privilege;
    uint64_t entry_point;         /* 相对文件起始（= 文件内偏移） */
    uint64_t ph_offset;           /* 程序头表起始文件偏移 */
    uint16_t ph_count;            /* 程序头表项数 */
    uint16_t _pad0;
    uint64_t sh_offset;
    uint16_t sh_count;
    uint16_t _pad1;
    uint64_t dep_count;
    uint64_t dep_strings_offset;
    uint32_t checksum;
    uint8_t  sig_type;            /* 0x01=Ed25519, 0x02=admin */
    uint8_t  sig_padding[3];
    uint32_t sig_length;
    /* ★ 第 18C 步：显式填充到 80 字节（与 Python 侧一致）。
     *   不使用 __attribute__((packed)) 的自然对齐，因为那样会移动
     *   字段偏移（sh_offset 从 28 移到 32），与 Python 写入位置不符。 */
    uint8_t  _reserved[12];
} __attribute__((packed));

/*
 * ★ 与 scripts/pe_to_obr.py 的 OBR_PHDR_SIZE 保持一致。
 *   原始 48 字节字段 + 8 字节显式填充 = 56。
 */
struct obr_phdr {
    uint32_t type;                /* OBR_PT_* */
    uint32_t flags;               /* OBR_PF_* */
    uint64_t offset;              /* 文件内偏移 */
    uint64_t vaddr;               /* 相对镜像基址的目标 RVA */
    uint64_t filesz;              /* 文件内有内容的字节数 */
    uint64_t memsz;               /* 内存中总字节数（含 BSS） */
    uint64_t align;
    /* ★ 第 18C 步：显式填充到 56 字节（与 Python 侧一致）。 */
    uint8_t  _reserved[8];
} __attribute__((packed));

/* ★ 编译期硬约束：防止未来再次出现结构体大小与构建工具不匹配。
 *   若这两个断言失败，不要修改断言——应修改 pe_to_obr.py 中的
 *   OBR_HDR_SIZE / OBR_PHDR_SIZE 使其与新结构体一致，或同步更新
 *   两侧。 */
_Static_assert(sizeof(struct obr_header) == 80,
               "obr_header must be exactly 80 bytes "
               "(must match pe_to_obr.py OBR_HDR_SIZE)");
_Static_assert(sizeof(struct obr_phdr) == 56,
               "obr_phdr must be exactly 56 bytes "
               "(must match pe_to_obr.py OBR_PHDR_SIZE)");

/* 校验头部与边界：0 = OK，负错误码 = 失败 */
int obr_validate_header(const struct obr_header *h, uint64_t image_size);

/* 最小加载：把 PT_LOAD 段复制到 load_base + vaddr。
 * 返回 0 成功；*out_entry 指向 load_base + entry_point。 */
int obr_load(const void *image, uint64_t image_size,
             uint64_t load_base,
             void **out_entry);

#endif /* OMNIBRIDGE_OBR_H */
/*===OmniBridgeOs/kernel/arch/x64/obr.h 结束===*/