/* kernel/arch/x64/compat_env.h
 * 兼容层环境欺骗引擎（第 18 步）。
 *
 * 关键约束（人工必须审查）：
 *   - PEB/auxv 模板仅存于共享区域，本步不映射到 0x7FFE0000。
 *   - 字段填充必须确定性（不使用随机数）。
 *   - 所有结构严格 packed，大小固定，便于区域布局。
 */
#ifndef OMNIBRIDGE_COMPAT_ENV_H
#define OMNIBRIDGE_COMPAT_ENV_H

#include <stdint.h>

/* Windows PEB 模板（占位语义）。 */
struct ob_peb_template {
    uint64_t image_base;
    uint64_t process_heap;
    uint64_t ldr;
    uint32_t os_major;
    uint32_t os_minor;
    uint32_t build;
    uint32_t _pad;
    uint64_t environment;
    uint64_t command_line;
    uint64_t dll_path;
    uint8_t  reserved[64];
} __attribute__((packed));

/* Linux auxv 条目。 */
struct ob_auxv_entry {
    uint64_t type;
    uint64_t value;
} __attribute__((packed));

struct ob_auxv_template {
    struct ob_auxv_entry entries[16];
    uint32_t count;
    uint32_t _pad;
} __attribute__((packed));

/* AT_* 常量（与 Linux 一致，供 compat_env_fill_auxv 使用） */
#define OB_AT_NULL     0
#define OB_AT_PAGESZ   6
#define OB_AT_UID      11
#define OB_AT_EUID     12
#define OB_AT_GID      13
#define OB_AT_EGID     14
#define OB_AT_CLKTCK   17
#define OB_AT_RANDOM   25

void compat_env_init(void);

/* 填充 PEB 模板。成功返回 0。 */
int compat_env_fill_peb(struct ob_peb_template *peb, uint64_t image_base,
                        uint64_t process_heap);

/* 填充 auxv 模板。成功返回 0。 */
int compat_env_fill_auxv(struct ob_auxv_template *auxv, uint32_t page_size,
                         uint32_t uid);

/* 打印模板内容（调试用）。任一参数可为 NULL。 */
void compat_env_dump(const struct ob_peb_template *peb,
                     const struct ob_auxv_template *auxv);

#endif /* OMNIBRIDGE_COMPAT_ENV_H */