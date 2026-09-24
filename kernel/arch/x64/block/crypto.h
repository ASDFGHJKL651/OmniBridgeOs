/*===OmniBridgeOs/kernel/arch/x64/block/crypto.h===*/
/*
 * OBFS 加密占位接口（第 18B 步）。
 *
 * 本步仅提供 ABI 稳定的空实现；实际加密算法留待后续步骤。
 */
#ifndef OMNIBRIDGE_BLOCK_CRYPTO_H
#define OMNIBRIDGE_BLOCK_CRYPTO_H

#include <stdint.h>

#define OBFS_CRYPTO_NONE   0
#define OBFS_CRYPTO_AES256 1   /* 预留 */

void crypto_init(void);

/* 设置加密算法与密钥。返回 -ENOSYS（本步未实现）。 */
int  crypto_set_key(uint8_t algo, const uint8_t *key, uint32_t key_len);

/* 当前算法。 */
uint8_t crypto_get_algo(void);

/* 加密/解密块（预留）。返回 -ENOSYS。 */
int  crypto_encrypt_block(uint64_t block_no, void *data, uint32_t len);
int  crypto_decrypt_block(uint64_t block_no, void *data, uint32_t len);

#endif /* OMNIBRIDGE_BLOCK_CRYPTO_H */
/*===OmniBridgeOs/kernel/arch/x64/block/crypto.h 结束===*/