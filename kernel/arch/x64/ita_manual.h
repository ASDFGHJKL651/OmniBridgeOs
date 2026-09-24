/*===OmniBridgeOs/kernel/arch/x64/ita_manual.h===*/
/*
 * ITA 第三信任层：管理员手动哈希白名单（第 17 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 条目仅存储在内核内存（全局静态数组），绝不持久化；
 *     进程重启或显式清空后失效。
 *   - 添加接口要求 caller 权限 9 且 ui_token_valid，且必须是
 *     非沙盒进程。
 *   - 每次添加均以 CRITICAL 级别写入审计日志，记录路径、哈希前缀、
 *     操作者 PID。
 *   - 所有操作在自旋锁保护下进行；查询接口在持有锁期间只拷贝出
 *     哈希值后即释放锁，验证计算在锁外完成。
 *
 * 与其它两层的关系（§5.8.4）：
 *   - 第一层（ita_fixed_hashes.h）固化于 .rodata，编译期决定。
 *   - 第二层（ita_sign.c）Ed25519 自签名，公钥由内核持有。
 *   - 第三层（本文件）运行时手动添加，仅当前会话有效。
 */
#ifndef OMNIBRIDGE_ITA_MANUAL_H
#define OMNIBRIDGE_ITA_MANUAL_H

#include <stdint.h>
#include "task.h"

#ifndef OB_ENOMEM
#define OB_ENOMEM (-12)
#endif

#define ITA_MANUAL_MAX_ENTRIES 64
#define ITA_MANUAL_PATH_MAX    256

struct ita_manual_entry {
    char     path[ITA_MANUAL_PATH_MAX];
    uint8_t  sha384[48];
    uint64_t added_by_pid;
    uint64_t tick;
};

/* 初始化（幂等）。 */
void ita_manual_init(void);

/* 添加/更新一条手动白名单。
 * 成功返回 0；
 * 权限不足或沙盒调用返回 OB_EPERM；
 * 条目满返回 OB_ENOMEM；
 * 参数错误返回 OB_EINVAL。
 * 若路径已存在，则更新哈希与操作者信息（不增加 count）。 */
int ita_manual_add(struct task_t *caller,
                   const char *path,
                   const uint8_t sha384[48]);

/* 查询：计算 buf 的 SHA-384 与白名单中 path 对应哈希比对。
 *   返回  0       匹配
 *   返回 -ENOENT  路径不在白名单
 *   返回 -EACCES  哈希不匹配 */
int ita_manual_verify(const char *path, const void *buf, uint64_t size);

/* 查询路径是否存在于白名单（不校验内容）。返回 1 / 0。 */
int ita_manual_contains(const char *path);

/* 清空所有条目（用于测试或会话重置）。 */
void ita_manual_clear(void);

/* 当前条目数。 */
uint32_t ita_manual_count(void);

/* 遍历所有条目（供 OShell `obctl trust list` 使用）。
 * 回调期间持有内部锁，回调不得再调用 ita_manual_* 接口。 */
typedef void (*ita_manual_iter_cb)(const struct ita_manual_entry *e,
                                   void *arg);
void ita_manual_iterate(ita_manual_iter_cb cb, void *arg);

#endif /* OMNIBRIDGE_ITA_MANUAL_H */
/*===OmniBridgeOs/kernel/arch/x64/ita_manual.h 结束===*/