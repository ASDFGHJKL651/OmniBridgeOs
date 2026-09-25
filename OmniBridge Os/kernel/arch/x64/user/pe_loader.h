/*===OmniBridgeOs/kernel/arch/x64/user/pe_loader.h===*/
/*
 * PE32+ (x86_64) 加载器 —— 第 20 步。
 *
 * 人工必须审查：
 *   - 所有结构严格 packed，字段布局符合 MS PE/COFF 规范。
 *   - 本步只支持 PE32+ EXE（非 DLL，非 PE32，非非 AMD64）。
 *   - 本步不处理重定位（要求 PE 使用首选 ImageBase）；
 *     若不能加载到首选基址，拒绝加载而不是静默错位。
 */
#ifndef OMNIBRIDGE_USER_PE_LOADER_H
#define OMNIBRIDGE_USER_PE_LOADER_H

#include <stdint.h>
#include "task.h"

/* ---------- 常量 ---------- */
#define PE_DOS_MAGIC         0x5A4Du
#define PE_NT_SIGNATURE      0x00004550u   /* "PE\0\0" */
#define PE_MACHINE_AMD64     0x8664u
#define PE_OPT_MAGIC_PE32P   0x020Bu
#define PE_DIR_IMPORT        1u
#define PE_DIR_IAT           12u

#define PE_SCN_MEM_EXECUTE   0x20000000u
#define PE_SCN_MEM_READ      0x40000000u
#define PE_SCN_MEM_WRITE     0x80000000u
#define PE_SCN_CNT_CODE      0x00000020u
#define PE_SCN_CNT_INIT_DATA 0x00000040u
#define PE_SCN_CNT_UNINIT    0x00000080u

#define PE_FILE_DLL          0x2000u
#define PE_FILE_EXECUTABLE   0x0002u

/*
 * 用户态 PE 加载基址。
 *
 * 人工必须审查：
 *   - PE 头部 ImageBase 通常为 0x140000000（Windows 默认），
 *     远大于 4GB，与内核 PML4[0] 的 0..4GB 2MB 恒等映射不冲突。
 *   - 若 PE 声称 ImageBase 与内核恒等映射区重叠，加载必须失败
 *     （本步不实现重定位，直接拒绝加载）。
 */
#define PE_USER_IMAGE_BASE   0x0000000140000000ULL
#define PE_USER_IMAGE_ALT    0x0000000800000000ULL

/* ---------- 磁盘结构 ---------- */
struct pe_dos_hdr {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} __attribute__((packed));

struct pe_coff_hdr {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} __attribute__((packed));

struct pe_opt_hdr64 {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
} __attribute__((packed));

struct pe_data_dir {
    uint32_t VirtualAddress;
    uint32_t Size;
} __attribute__((packed));

struct pe_section_hdr {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} __attribute__((packed));

struct pe_import_desc {
    uint32_t OriginalFirstThunk;
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;
} __attribute__((packed));

struct pe_import_by_name {
    uint16_t Hint;
    char     Name[1];
} __attribute__((packed));

/* ---------- 解析结果 ---------- */
struct pe_info {
    uint64_t entry_rva;
    uint64_t image_base_pref;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t stack_reserve;
    uint64_t stack_commit;
    uint64_t heap_reserve;
    uint64_t heap_commit;
    uint16_t num_sections;
    uint16_t _pad;
};

/* ---------- 接口 ---------- */
int pe_detect(const void *buf, uint64_t size);
int pe_parse_info(const void *buf, uint64_t size, struct pe_info *out);

int pe_load_into_task(struct task_t *t,
                      const uint8_t *img, uint64_t size,
                      uint64_t *entry_va, uint64_t *image_base,
                      uint32_t *size_of_image);

int pe_resolve_imports(struct task_t *t,
                       const uint8_t *img, uint64_t size,
                       uint64_t image_base,
                       uint64_t (*stub_lookup)(const char *dll,
                                                const char *func));

#endif /* OMNIBRIDGE_USER_PE_LOADER_H */