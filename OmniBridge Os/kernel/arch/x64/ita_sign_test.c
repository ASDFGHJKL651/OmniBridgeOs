/*===OmniBridgeOs/kernel/arch/x64/ita_sign_test.c===*/
#include "ita_sign_test.h"
#include "ita_sign.h"
#include "ed25519.h"
#include "obr.h"
#include "uel.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[ITA-SIGN-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[ITA-SIGN-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

/* 用当前内核持有的公钥验证签名。
 * 若 ita_sign_ready() == 0，返回 0（视为失败）。 */
static int ita_verify_signature_self(const void *msg, uint64_t len,
                                     const uint8_t sig[64])
{
    uint8_t pub[32];
    if (ita_sign_export_pubkey(pub) != 0) return 0;
    return ita_verify_signature(pub, msg, len, sig);
}

/* 在栈上构造一个最小 .obr（未签名） */
static void build_minimal_obr(uint8_t *img, uint64_t img_size,
                              uint8_t sig_type, uint32_t sig_len)
{
    for (uint64_t i = 0; i < img_size; ++i) img[i] = 0;

    struct obr_header *h = (struct obr_header *)img;
    h->magic      = OBR_MAGIC;
    h->version    = OBR_VERSION;
    h->arch       = OBR_ARCH_X64;
    h->min_privilege = 0;
    h->entry_point   = sizeof(struct obr_header) + sizeof(struct obr_phdr);
    h->ph_offset     = sizeof(struct obr_header);
    h->ph_count      = 1;
    h->sig_type      = sig_type;
    h->sig_length    = sig_len;

    struct obr_phdr *ph = (struct obr_phdr *)(img + sizeof(struct obr_header));
    ph->type   = OBR_PT_LOAD;
    ph->flags  = OBR_PF_R | OBR_PF_X;
    ph->offset = sizeof(struct obr_header) + sizeof(struct obr_phdr);
    ph->vaddr  = 0;
    ph->filesz = 8;
    ph->memsz  = 8;
    ph->align  = 0;

    uint64_t *payload = (uint64_t *)(img + ph->offset);
    *payload = 0xDEADBEEFCAFEBABEULL;
}

void ita_sign_test(void)
{
    failures = 0;
    serial_printf("[ITA-SIGN-TEST] === begin ===\n");

    check("ita_sign_ready() == 1", ita_sign_ready() == 1);
    if (!ita_sign_ready()) {
        serial_printf("[ITA-SIGN-TEST] sign subsystem not ready, "
                      "skipping end-to-end\n");
        if (failures == 0) {
            serial_printf("[ITA-SIGN-TEST] selftest OK\n");
        } else {
            serial_printf("[ITA-SIGN-TEST] selftest FAILED: %d case(s)\n",
                          failures);
        }
        serial_printf("[ITA-SIGN-TEST] === end ===\n");
        return;
    }

    /* 1) 公钥稳定性 */
    {
        uint8_t p1[32], p2[32];
        int ok = (ita_sign_export_pubkey(p1) == 0)
              && (ita_sign_export_pubkey(p2) == 0);
        int same = 1;
        for (int i = 0; i < 32; ++i) if (p1[i] != p2[i]) { same = 0; break; }
        check("pubkey stable across calls", ok && same);
    }

    /* 2) sign/verify 往返 */
    {
        const char *msg = "hello ITA2";
        uint8_t sig[64];
        int rc = ita_sign_hash(msg, 10, sig);
        check("sign/verify roundtrip",
              rc == 0 &&
              ita_verify_signature_self(msg, 10, sig));
    }

    /* 3) 篡改签名字节 */
    {
        const char *msg = "hello ITA2";
        uint8_t sig[64];
        ita_sign_hash(msg, 10, sig);
        sig[0] ^= 0x01;
        int ok = !ita_verify_signature_self(msg, 10, sig);
        check("tampered signature rejected", ok);
    }

    /* 4) 完整 .obr 场景：未签名 */
    {
        static uint8_t img[256];
        static uint8_t target[64];
        build_minimal_obr(img, sizeof(img), 0x00, 0);
        struct uel_load_result res;
        int rc = uel_load(img, sizeof(img),
                          (uint64_t)(uintptr_t)target, 0, &res);
        check("unsig .obr loaded without VERIFIED",
              rc == 0 && !(res.flags & UEL_FLAG_OBR_SIGNATURE_OK));
    }

    /* 5) 完整 .obr 场景：带签名 */
    {
        static uint8_t img[512];
        build_minimal_obr(img, sizeof(img) - 64, 0x01, 64);

        /* 对 [0, size-64) 的字节签名 */
        uint64_t msg_len = sizeof(img) - 64;
        uint8_t sig[64];
        int rc = ita_sign_hash(img, msg_len, sig);
        if (rc == 0) {
            for (int i = 0; i < 64; ++i) img[msg_len + i] = sig[i];
        }
        static uint8_t target[64];
        struct uel_load_result res;
        int rc2 = uel_load(img, sizeof(img),
                           (uint64_t)(uintptr_t)target, 0, &res);
        check("signed .obr loaded with SIGNATURE_OK",
              rc == 0 && rc2 == 0 &&
              (res.flags & UEL_FLAG_OBR_SIGNATURE_OK));
    }

    /* 6) 篡改镜像内容 */
    {
        static uint8_t img[512];
        build_minimal_obr(img, sizeof(img) - 64, 0x01, 64);
        uint64_t msg_len = sizeof(img) - 64;
        uint8_t sig[64];
        ita_sign_hash(img, msg_len, sig);
        for (int i = 0; i < 64; ++i) img[msg_len + i] = sig[i];

        /* 篡改 payload 的最后一字节（phdr 结束位置 = 80 + 56 = 136；
         * payload 从 136 开始，但 obr_load 只复制 filesz=8，
         * 这里篡改 payload 内的字节） */
        img[80 + 56 + 3] ^= 0xFF;

        static uint8_t target[64];
        struct uel_load_result res;
        int rc = uel_load(img, sizeof(img),
                          (uint64_t)(uintptr_t)target, 0, &res);
        check("tampered .obr rejected (-EACCES)", rc != 0);
    }

    if (failures == 0) {
        serial_printf("[ITA-SIGN-TEST] selftest OK\n");
    } else {
        serial_printf("[ITA-SIGN-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[ITA-SIGN-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/ita_sign_test.c 结束===*/