/* kernel/arch/x64/ita_sign.h
 * ITA 第二层：系统绑定自签名（Ed25519）。
 *
 * 关键约束（人工必须审查）：
 *   - 私钥仅在本模块的 .c 内可见；任何函数不返回 &g_priv。
 *   - ita_sign_init 的幂等性：多次调用只生成一次密钥。
 *   - 失败路径：g_inited = -1，ita_sign_ready() 返回 0，所有 sig_type==0x01
 *     的 .obr 被 uel_load_obr 拒绝并审计。
 */
#ifndef OMNIBRIDGE_ITA_SIGN_H
#define OMNIBRIDGE_ITA_SIGN_H

#include <stdint.h>
#include "ed25519.h"

/* 0 = 成功；-1 = 不可用（RNG 失败 / Ed25519 KAT 失败） */
int ita_sign_init(void);

/* 1 = 已就绪；0 = 不可用 */
int ita_sign_ready(void);

/* 导出公钥（公开信息，安全）。成功返回 0。 */
int ita_sign_export_pubkey(uint8_t out[32]);

/* 对任意 buffer 签名。调用者负责保证 len <= 16 MiB。成功返回 0。 */
int ita_sign_hash(const void *data, uint64_t len,
                  uint8_t sig[ED25519_SIG_LEN]);

/* 验证。返回 1 = 通过；0 = 失败。 */
int ita_verify_signature(const uint8_t pub[32],
                         const void *data, uint64_t len,
                         const uint8_t sig[ED25519_SIG_LEN]);

#endif /* OMNIBRIDGE_ITA_SIGN_H */