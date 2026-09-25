/*===OmniBridgeOs/kernel/arch/x64/uel.h===*/
#ifndef OMNIBRIDGE_UEL_H
#define OMNIBRIDGE_UEL_H

#include <stdint.h>
#include "task.h"

/*
 * 统一可执行加载器（UEL）—— 第 14 步版本。
 *
 * 本步 UEL 做四件事：
 *   1) 通过魔数识别格式（OBR / PE / ELF）；
 *   2) 解析 .obr 头与 phdr；
 *   3) **验证 Ed25519 签名（若 sig_type == 0x01）**；
 *   4) 将每个 PT_LOAD 段复制到 load_base + vaddr，并清零 BSS。
 *
 * 本步 UEL **不**做：
 *   - 建立页表映射；
 *   - 修改 CR3；
 *   - 处理 PT_DYNAMIC / PT_INTERP 依赖；
 *   - 处理重定位；
 *   - 跳转到 entry。
 */

#define UEL_MAX_IMAGE_SIZE (16ULL * 1024 * 1024)   /* 16 MiB */
#define UEL_MAX_LOAD_SIZE  (64ULL * 1024 * 1024)   /* 64 MiB */

enum uel_format {
    UEL_FMT_UNKNOWN = 0,
    UEL_FMT_OBR     = 1,
    UEL_FMT_PE      = 2,
    UEL_FMT_ELF     = 3,
};

struct uel_load_result {
    enum uel_format format;
    uint64_t entry;        /* 入口虚拟地址（= load_base + entry_point） */
    uint64_t load_base;    /* 调用方提供的加载基址 */
    uint64_t total_size;   /* 镜像总大小 */
    uint32_t flags;        /* UEL_FLAG_* */
};

/* 旧的 ITA 占位 flag（已废弃，第 14 步起不再使用） */
#define UEL_FLAG_OBR_VERIFIED        0x01u

/* 段特征 flag */
#define UEL_FLAG_OBR_HAS_DYNAMIC     0x02u   /* 含 PT_DYNAMIC（仅标记） */
#define UEL_FLAG_OBR_HAS_INTERP      0x04u   /* 含 PT_INTERP（仅标记） */

/* 第 14 步新增：签名验证相关 flag */
#define UEL_FLAG_OBR_SIGNATURE_OK    0x08u   /* Ed25519 验证通过 */
#define UEL_FLAG_OBR_SIGNATURE_SKIP  0x10u   /* 签名跳过（保留） */

/* 通过魔数识别格式。buf 至少 4 字节；size < 4 时返回 UEL_FMT_UNKNOWN。 */
enum uel_format uel_detect_format(const void *buf, uint64_t size);

/*
 * 加载镜像到 load_base。
 *   caller == NULL 表示内核引导阶段（视为 priv=9，允许任何 min_privilege）。
 *   返回 0 成功；负错误码失败。
 *
 *   签名验证（第 14 步新增）：
 *     - sig_type == 0x00：未签名，允许（打印 WARN），沙盒禁止。
 *     - sig_type == 0x01：Ed25519 系统绑定签名，使用内核持有的公钥
 *                        验证文件末尾 64 字节；失败返回 -EACCES；沙盒禁止。
 *     - sig_type == 0x02：管理员手动标记，本步未实现，返回 -ENOSYS。
 */
int uel_load(const void *buf, uint64_t size,
             uint64_t load_base,
             struct task_t *caller,
             struct uel_load_result *out);

/*
 * 便捷入口：从 VFS 打开文件、读入内核缓冲区、调用 uel_load。
 * 内部使用 kmalloc/kfree，不泄漏。
 */
int uel_load_path(const char *path,
                  uint64_t load_base,
                  struct task_t *caller,
                  struct uel_load_result *out);

#endif /* OMNIBRIDGE_UEL_H */
/*===OmniBridgeOs/kernel/arch/x64/uel.h 结束===*/