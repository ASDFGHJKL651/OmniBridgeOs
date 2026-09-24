/* kernel/arch/x64/ed25519.h
 * RFC 8032 Ed25519 纯软件实现（无外部库）。
 *
 * 关键约束（人工必须审查）：
 *   - 有限域 p = 2^255 - 19，5x51 位 limb 表示。
 *   - 群运算使用扩展坐标 (X:Y:Z:T)，曲线 -x^2+y^2 = 1+d*x^2*y^2。
 *   - 拒绝 non-canonical 编码（S < L；y 坐标 < p）。
 *   - 输入校验顺序：先解码 R/S/A，再做 [S]B = R + [k]A 判定。
 *   - 不使用任何 libc 函数。
 *
 * 安全声明（人工必须审查）：
 *   - 本实现严格实现 RFC 8032 §5.1 的算法与常量。
 *   - 标量乘使用朴素双倍-累加（存在标量位数分支）；对于本内核阶段
 *     （无攻击者在同一 CPU 并行执行），可接受。一旦引入用户态并行
 *     执行，必须替换为窗口法 + 常量时间（如 ref10 的 ge_scalarmult_base
 *     预计算表）。
 *   - 未实现侧信道盲化。
 */
#ifndef OMNIBRIDGE_ED25519_H
#define OMNIBRIDGE_ED25519_H

#include <stdint.h>

#define ED25519_PUBKEY_LEN   32
#define ED25519_PRIVKEY_LEN  64   /* seed(32) || pubkey(32) */
#define ED25519_SEED_LEN     32
#define ED25519_SIG_LEN      64

/* 从 32 字节 seed 派生密钥对。
 * priv 输出为 64 字节：低 32 字节 = seed，高 32 字节 = pubkey。
 * 返回 0 成功。 */
int ed25519_keypair_from_seed(const uint8_t seed[ED25519_SEED_LEN],
                              uint8_t priv[ED25519_PRIVKEY_LEN],
                              uint8_t pub[ED25519_PUBKEY_LEN]);

/* 对 msg 签名。msg 可为 NULL 当且仅当 msg_len == 0。
 * 返回 0 成功；-1 表示参数非法。 */
int ed25519_sign(const uint8_t priv[ED25519_PRIVKEY_LEN],
                 const void *msg, uint64_t msg_len,
                 uint8_t sig[ED25519_SIG_LEN]);

/* 验证签名。返回 1 = 通过；0 = 失败（含所有错误情况）。 */
int ed25519_verify(const uint8_t pub[ED25519_PUBKEY_LEN],
                   const void *msg, uint64_t msg_len,
                   const uint8_t sig[ED25519_SIG_LEN]);

#endif /* OMNIBRIDGE_ED25519_H */