/*===OmniBridgeOs/kernel/arch/x64/block/crypto.c===*/
#include "crypto.h"
#include "serial.h"

static uint8_t g_algo = OBFS_CRYPTO_NONE;

void crypto_init(void)
{
    g_algo = OBFS_CRYPTO_NONE;
    serial_printf("[CRYPTO] init: algo=NONE (stub)\n");
}

int crypto_set_key(uint8_t algo, const uint8_t *key, uint32_t key_len)
{
    (void)key; (void)key_len;
    if (algo == OBFS_CRYPTO_NONE) {
        g_algo = OBFS_CRYPTO_NONE;
        return 0;
    }
    return -38;   /* -ENOSYS */
}

uint8_t crypto_get_algo(void) { return g_algo; }

int crypto_encrypt_block(uint64_t block_no, void *data, uint32_t len)
{
    (void)block_no; (void)data; (void)len;
    return -38;
}

int crypto_decrypt_block(uint64_t block_no, void *data, uint32_t len)
{
    (void)block_no; (void)data; (void)len;
    return -38;
}
/*===OmniBridgeOs/kernel/arch/x64/block/crypto.c 结束===*/