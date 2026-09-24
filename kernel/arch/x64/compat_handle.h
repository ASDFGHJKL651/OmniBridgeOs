/* kernel/arch/x64/compat_handle.h
 * 兼容层句柄表（第 18 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 所有对 entries 的修改都在 ht->lock 保护下。
 *   - 0/1/2 保留给 stdin/stdout/stderr，创建时预置。
 *   - 句柄从 COMPAT_HANDLE_BASE 开始递增；回绕时扫描空闲槽。
 */
#ifndef OMNIBRIDGE_COMPAT_HANDLE_H
#define OMNIBRIDGE_COMPAT_HANDLE_H

#include <stdint.h>
#include "spinlock.h"

#define COMPAT_HANDLE_MAX      256
#define COMPAT_HANDLE_BASE     0x100u
#define COMPAT_INVALID_HANDLE  0xFFFFFFFFu

/* 与 permission.h 保持一致 */
#ifndef OB_RES_FILE
#define OB_RES_FILE    0
#endif
#ifndef OB_RES_MEMORY
#define OB_RES_MEMORY  1
#endif
#ifndef OB_RES_PROCESS
#define OB_RES_PROCESS 2
#endif
#ifndef OB_RES_IPC
#define OB_RES_IPC     3
#endif

struct compat_handle_entry {
    uint32_t handle;
    uint32_t _pad;
    uint64_t kernel_handle;
    uint32_t type;
    uint32_t flags;
    uint8_t  in_use;
    uint8_t  _pad2[7];
};

struct compat_handle_table {
    struct compat_handle_entry entries[COMPAT_HANDLE_MAX];
    uint32_t next_handle;
    uint32_t count;
    spinlock_t lock;
};

struct compat_handle_table *compat_handle_table_create(void);
void     compat_handle_table_destroy(struct compat_handle_table *ht);

uint32_t compat_handle_alloc(struct compat_handle_table *ht,
                             uint64_t kernel_handle, uint32_t type,
                             uint32_t flags);
int      compat_handle_free(struct compat_handle_table *ht, uint32_t handle);
int      compat_handle_lookup(struct compat_handle_table *ht, uint32_t handle,
                              struct compat_handle_entry *out);
uint32_t compat_handle_count(struct compat_handle_table *ht);

#endif /* OMNIBRIDGE_COMPAT_HANDLE_H */