/* kernel/arch/x64/sha256.h
 * FIPS 180-4 SHA-256 自实现。
 *
 * 关键约束（人工必须审查）：
 *   - 不依赖任何外部库。
 *   - 常量时间：算法内部无数据相关分支。
 *   - 流式 API：sha256_update 可多次调用。
 *   - 输出 32 字节。
 *
 * 用途：替代步骤 17 art.c 中的 SHA-384 截断，作为 ART 键散列。
 */
#ifndef OMNIBRIDGE_SHA256_H
#define OMNIBRIDGE_SHA256_H

#include <stdint.h>
#include <stddef.h>

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;          /* 已处理的总位数（uint64 足够） */
    uint8_t  buffer[64];
    uint32_t buffer_len;
};

void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, uint64_t len);
void sha256_final(struct sha256_ctx *ctx, uint8_t out[32]);

/* 一次性计算便捷接口 */
void sha256(const void *data, uint64_t len, uint8_t out[32]);

#endif /* OMNIBRIDGE_SHA256_H */