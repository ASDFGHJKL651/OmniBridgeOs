/*===OmniBridgeOs/kernel/arch/x64/sha256_test.c===*/
#include "sha256_test.h"
#include "sha256.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[SHA256-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[SHA256-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static int mem_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

/* FIPS 180-4 §B.1 / §B.2 官方向量 */
static const uint8_t VEC_EMPTY[32] = {
    0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
    0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
    0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
    0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55,
};

static const uint8_t VEC_ABC[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
    0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
    0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
};

void sha256_test(void)
{
    uint8_t out[32];
    failures = 0;

    /* 1) 空字符串 */
    sha256("", 0, out);
    check("empty string", mem_eq(out, VEC_EMPTY, 32));

    /* 2) "abc" */
    sha256("abc", 3, out);
    check("abc", mem_eq(out, VEC_ABC, 32));

    /* 3) 流式一致性 */
    {
        uint8_t buf[300];
        for (int i = 0; i < 300; ++i) buf[i] = (uint8_t)(i & 0xFF);

        uint8_t ref[32];
        sha256(buf, 300, ref);

        struct sha256_ctx ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, buf, 1);
        sha256_update(&ctx, buf + 1, 62);
        sha256_update(&ctx, buf + 63, 237);
        uint8_t streamed[32];
        sha256_final(&ctx, streamed);
        check("streamed update matches one-shot",
              mem_eq(ref, streamed, 32));
    }

    /* 4) 块边界 55/56/57 与 63/64/65 */
    {
        uint8_t buf[128];
        for (int i = 0; i < 128; ++i) buf[i] = (uint8_t)(i + 1);

        uint8_t r55[32], r56[32], r57[32];
        uint8_t r63[32], r64[32], r65[32];
        sha256(buf, 55, r55);
        sha256(buf, 56, r56);
        sha256(buf, 57, r57);
        sha256(buf, 63, r63);
        sha256(buf, 64, r64);
        sha256(buf, 65, r65);

        check("boundary 55 != 56", !mem_eq(r55, r56, 32));
        check("boundary 56 != 57", !mem_eq(r56, r57, 32));
        check("boundary 63 != 64", !mem_eq(r63, r64, 32));
        check("boundary 64 != 65", !mem_eq(r64, r65, 32));
    }

    if (failures == 0) {
        serial_printf("[SHA256-TEST] selftest OK\n");
    } else {
        serial_printf("[SHA256-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}