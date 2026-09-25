/*
 * SHA-384 自实现（FIPS 180-4）。
 *
 * 关键约束（人工必须审查）：
 *   - 不依赖任何外部库（无 OpenSSL、无 libcrypto）。
 *   - 常量时间：算法内部无数据相关分支。
 *   - 流式 API：sha384_update 可多次调用。
 *   - 输出 48 字节（384 bit = 6 x 64-bit 截断）。
 *
 * SHA-384 是 SHA-512 的截断版本：使用不同的初始向量，输出时只取前
 * 6 个 64 位状态字（前 48 字节）。
 */
#ifndef OMNIBRIDGE_SHA384_H
#define OMNIBRIDGE_SHA384_H

#include <stdint.h>
#include <stddef.h>

struct sha384_ctx {
    uint64_t state[8];
    uint64_t bitlen[2];      /* 已处理的总位数（128 位） */
    uint8_t  buffer[128];
    uint32_t buffer_len;
};

void sha384_init(struct sha384_ctx *ctx);
void sha384_update(struct sha384_ctx *ctx, const void *data, uint64_t len);
void sha384_final(struct sha384_ctx *ctx, uint8_t out[48]);

/* 一次性计算便捷接口 */
void sha384(const void *data, uint64_t len, uint8_t out[48]);

#endif /* OMNIBRIDGE_SHA384_H */