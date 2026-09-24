/*===OmniBridgeOs/kernel/arch/x64/compat_preload.h===*/
/*
 * 兼容层预加载（第 18 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 仅 PID 1（Init）可调用 compat_preload_run()。
 *   - 区域结构持有对 PEB/auxv 模板、ART 表基址的引用。
 *   - 第 18 步起，区域中实际填充 PEB/auxv 模板内容。
 */
#ifndef OMNIBRIDGE_COMPAT_PRELOAD_H
#define OMNIBRIDGE_COMPAT_PRELOAD_H

#include <stdint.h>
#include "task.h"
#include "shm.h"

#define COMPAT_REGION_SIZE (4ULL * 1024)

struct compat_preload_region {
    struct shm_region *shm;
    void              *art_table;
    void              *peb_template;
    void              *auxv_template;
    uint64_t           size;
    uint32_t           refcount;
    uint32_t           _pad;
};

void compat_preload_init(void);
int  compat_preload_run(struct task_t *caller);
void compat_preload_inherit(struct task_t *child, struct task_t *parent);
void compat_preload_release(struct task_t *t);
void *compat_preload_region_ptr(void);

#endif /* OMNIBRIDGE_COMPAT_PRELOAD_H */