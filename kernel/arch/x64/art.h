/*===OmniBridgeOs/kernel/arch/x64/art.h===*/
/*
 * API 重定向表（ART）—— 第 18 步版本：键改为 SHA-256 截断。
 *
 * 关键约束（人工必须审查）：
 *   - 表内容仅驻留内核内存（由 pmm 分配连续物理页 + DirectMap 访问）。
 *   - 键为 API 名称 SHA-256 前 8 字节（big-endian 组合）。
 *   - 名称字符串用于防碰撞二次校验。
 *   - 所有操作在自旋锁保护下进行。
 *   - 表容量固定 ART_MAX_ENTRIES；超出返回 OB_ENOMEM。
 */
#ifndef OMNIBRIDGE_ART_H
#define OMNIBRIDGE_ART_H

#include <stdint.h>

#ifndef OB_ENOMEM
#define OB_ENOMEM (-12)
#endif
#ifndef OB_EPERM
#define OB_EPERM  (-1)
#endif
#ifndef OB_EINVAL
#define OB_EINVAL (-22)
#endif

struct task_t;

#define ART_MAX_ENTRIES 4096
#define ART_NAME_MAX    128

struct art_entry {
    uint64_t key;                  /* SHA-256 前 8 字节（big-endian 组合） */
    char     name[ART_NAME_MAX];   /* 原始 API 名称（防碰撞校验） */
    void    *func_ptr;             /* 模拟函数指针 */
    uint32_t flags;                /* 调用方自定义标志 */
    uint32_t _pad;
};

struct art_table_stats {
    uint32_t count;
    uint64_t generation;
};

void art_init(void);
int  art_register(const char *api_name, void *func_ptr, uint32_t flags);
int  art_lookup(const char *api_name, struct art_entry *out);
void art_walk(void (*cb)(const struct art_entry *e, void *arg), void *arg);
uint32_t art_count(void);
uint64_t art_hash_name(const char *api_name);
void art_stats(struct art_table_stats *out);

/* ★ 第 18 步：返回表的 DirectMap 虚拟地址；未初始化返回 0。 */
uint64_t art_base_addr(void);

/* ★ 第 18 步：动态扩展接口（需权限 ≥ 6）。 */
int ob_register_api_alias(struct task_t *caller,
                          const char *api_name,
                          void *func_ptr,
                          uint32_t flags,
                          const char *target_path);

#endif /* OMNIBRIDGE_ART_H */