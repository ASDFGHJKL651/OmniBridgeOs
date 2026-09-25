/* kernel/arch/x64/sha512.h
 * FIPS 180-4 SHA-512 自实现。
 *
 * 与 sha384.c 的差异：
 *   - 不同的初始 IV（§5.3.3）
 *   - 输出 64 字节（8 个 64 位字全部输出）
 *   - 轮常量 K[0..79] 与 SHA-384 完全相同（§4.2.3）
 *
 * 关键约束（人工必须审查）：
 *   - 不依赖任何外部库。
 *   - 常量时间：算法内部无数据相关分支。
 *   - 流式 API：sha512_update 可多次调用。
 */
#ifndef OMNIBRIDGE_SHA512_H
#define OMNIBRIDGE_SHA512_H

#include <stdint.h>
#include <stddef.h>

struct sha512_ctx {
    uint64_t state[8];
    uint64_t bitlen[2];      /* 已处理的总位数（128 位） */
    uint8_t  buffer[128];
    uint32_t buffer_len;
};

void sha512_init(struct sha512_ctx *ctx);
void sha512_update(struct sha512_ctx *ctx, const void *data, uint64_t len);
void sha512_final(struct sha512_ctx *ctx, uint8_t out[64]);

/* 一次性计算便捷接口 */
void sha512(const void *data, uint64_t len, uint8_t out[64]);

#endif /* OMNIBRIDGE_SHA512_H */