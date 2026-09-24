/*===OmniBridgeOs/kernel/arch/x64/ita.h===*/
/*
 * ITA（Internal Trust Anchor）第一层：内核固化哈希白名单。
 *
 * 关键约束（人工必须审查）：
 *   - 固化哈希必须 const，位于 .rodata。
 *   - ita_verify_fixed 内部**不 panic**（由调用方决定）。
 *   - ita_verify_all_critical 内部**直接 panic**（§5.8.1）。
 *   - 不使用 kmalloc 存储哈希。
 *
 * 第 17 步追加：
 *   - 第三层手动白名单查询接口。
 *   - 统一入口 ita_verify_any：先查第一层，未命中再查第三层。
 *   - ita_init 中初始化手动白名单子系统。
 */
#ifndef OMNIBRIDGE_ITA_H
#define OMNIBRIDGE_ITA_H

#include <stdint.h>

struct ita_fixed_entry {
    const char *path;
    uint8_t     sha384[48];
};

/* 白名单由 ita_fixed_hashes.h 定义 */
extern const struct ita_fixed_entry g_ita_fixed_whitelist[];
extern const uint32_t              g_ita_fixed_count;

/* 初始化：一致性自检（不修改白名单）；并初始化手动白名单子系统。 */
void ita_init(void);

/*
 * 检查给定 buffer 是否匹配指定路径的固化哈希。
 *   返回  0       匹配
 *   返回 -EACCES  不匹配
 *   返回 -ENOENT  路径不在白名单
 */
int ita_verify_fixed(const char *path, const void *buf, uint64_t size);

/*
 * 启动早期调用：验证首批 4 个核心程序的固化哈希。
 * 失败即 panic（不返回）。
 */
int ita_verify_all_critical(void);

/* 白名单条目数 */
uint32_t ita_fixed_whitelist_count(void);

/* ---------- 第 17 步新增 ---------- */

/* 第三层查询（转发到 ita_manual_verify）。 */
int ita_verify_manual(const char *path, const void *buf, uint64_t size);

/*
 * 统一入口：
 *   - 若路径命中第一层固化白名单：
 *       哈希匹配 → 返回 0；不匹配 → 返回 -EACCES（不再查第三层）。
 *   - 若路径未命中第一层：
 *       查第三层手动白名单；命中校验 → 0/-EACCES；
 *       若也未命中 → 返回 -ENOENT。
 */
int ita_verify_any(const char *path, const void *buf, uint64_t size);

#endif /* OMNIBRIDGE_ITA_H */
/*===OmniBridgeOs/kernel/arch/x64/ita.h 结束===*/