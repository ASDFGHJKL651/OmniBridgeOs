/* kernel/arch/x64/ita_sign.c
 * ITA 第二层实现。
 *
 * 私钥保护（人工必须审查）：
 *   - g_priv 为 static，仅本文件可见；
 *   - 无任何函数返回 &g_priv；
 *   - 初始化后立即清零栈上 seed 副本；
 *   - audit / serial_printf 从不打印私钥。
 *
 * 持久化（本步限制）：
 *   本步根文件系统为 tmpfs，无 ita_public_key 字段，因此走
 *   "临时密钥"路径。每次启动公钥都会变，无法做跨启动的签名验证。
 *   真正的持久化需要 OBFS 挂载（步骤 20+）。
 */
#include "ita_sign.h"
#include "rng.h"
#include "ed25519.h"
#include "serial.h"

/* 私钥：仅本文件可见 */
static uint8_t g_priv[ED25519_PRIVKEY_LEN];
static uint8_t g_pub [ED25519_PUBKEY_LEN];
static int     g_inited = 0;      /* 0=未初始化, 1=就绪, -1=不可用 */

/* 简单清零 */
static void secure_zero(void *p, uint64_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    for (uint64_t i = 0; i < n; ++i) b[i] = 0;
}

/* RFC 8032 §7.1 TEST 1 的 seed / pubkey / sig（用于自检） */
static const uint8_t KAT1_SEED[32] = {
    0x9d,0x61,0xb1,0x9d,0xef,0xfd,0x5a,0x60,
    0xba,0x84,0x4a,0xf4,0x92,0xec,0x2c,0xc4,
    0x44,0x49,0xc5,0x69,0x7b,0x32,0x69,0x19,
    0x70,0x3b,0xac,0x03,0x1c,0xae,0x7f,0x60,
};
static const uint8_t KAT1_PUB[32] = {
    0xd7,0x5a,0x98,0x01,0x82,0xb1,0x0a,0xb7,
    0xd5,0x4b,0xfe,0xd3,0xc9,0x64,0x07,0x3a,
    0x0e,0xe1,0x72,0xf3,0xda,0xa6,0x23,0x25,
    0xaf,0x02,0x1a,0x68,0xf7,0x07,0x51,0x1a,
};

/* 快速自检：KAT1 派生公钥是否匹配 */
static int ed25519_selfcheck(void)
{
    uint8_t priv[ED25519_PRIVKEY_LEN];
    uint8_t pub[ED25519_PUBKEY_LEN];
    if (ed25519_keypair_from_seed(KAT1_SEED, priv, pub) != 0) {
        secure_zero(priv, sizeof(priv));
        return 0;
    }
    int ok = 1;
    for (int i = 0; i < 32; ++i) {
        if (pub[i] != KAT1_PUB[i]) { ok = 0; break; }
    }
    secure_zero(priv, sizeof(priv));
    return ok;
}

int ita_sign_init(void)
{
    if (g_inited == 1)  return 0;
    if (g_inited == -1) return -1;

    rng_init();

    /* 先做一次算法自检：KAT1 派生公钥必须匹配。若不匹配，
     * 立即禁用 ITA 第二层（降级路径）。 */
    if (!ed25519_selfcheck()) {
        serial_printf("[ITA2] WARN: Ed25519 self-test failed, "
                      "sign subsystem disabled\n");
        g_inited = -1;
        return -1;
    }

    serial_printf("[ITA2] init: generating ephemeral keypair "
                  "(no OBFS root)\n");

    uint8_t seed[32];
    if (rng_bytes(seed, sizeof(seed)) != 0) {
        serial_printf("[ITA2] WARN: RNG failed, sign subsystem disabled\n");
        g_inited = -1;
        return -1;
    }

    if (ed25519_keypair_from_seed(seed, g_priv, g_pub) != 0) {
        secure_zero(seed, sizeof(seed));
        g_inited = -1;
        return -1;
    }

    /* 立即清零栈上 seed 副本 */
    secure_zero(seed, sizeof(seed));

    serial_printf("[ITA2] WARN: OBFS root not present, using ephemeral key\n");
    g_inited = 1;
    return 0;
}

int ita_sign_ready(void)
{
    return g_inited == 1;
}

int ita_sign_export_pubkey(uint8_t out[32])
{
    if (g_inited != 1) return -1;
    for (int i = 0; i < 32; ++i) out[i] = g_pub[i];
    return 0;
}

int ita_sign_hash(const void *data, uint64_t len,
                  uint8_t sig[ED25519_SIG_LEN])
{
    if (g_inited != 1) return -1;
    if (!data && len > 0) return -1;
    if (ed25519_sign(g_priv, data, len, sig) != 0) return -1;
    return 0;
}

int ita_verify_signature(const uint8_t pub[32],
                         const void *data, uint64_t len,
                         const uint8_t sig[ED25519_SIG_LEN])
{
    if (!pub || !sig) return 0;
    return ed25519_verify(pub, data, len, sig);
}