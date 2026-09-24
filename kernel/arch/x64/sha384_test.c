/*===OmniBridgeOs/kernel/arch/x64/sha384_test.c===*/
#include "sha384_test.h"
#include "sha384.h"
#include "serial.h"

/*
 * 使用 NIST FIPS 180-4 官方测试向量。
 *
 * 注意：规范提示词中 SHA-384("") 的向量有误（给的是 "abc" 的值），
 *       本测试使用 FIPS 180-4 附录的正确值：
 *         SHA-384("")    = 38b060a751ac9638...
 *         SHA-384("abc") = cb00753f45a35e8b...
 *
 * 严禁调用 kmalloc / pmm_* / task_create。
 */

static const uint8_t VEC_EMPTY[48] = {
    0x38,0xb0,0x60,0xa7,0x51,0xac,0x96,0x38,
    0x4c,0xd9,0x32,0x7e,0xb1,0xb1,0xe3,0x6a,
    0x21,0xfd,0xb7,0x11,0x14,0xbe,0x07,0x43,
    0x4c,0x0c,0xc7,0xbf,0x63,0xf6,0xe1,0xda,
    0x27,0x4e,0xde,0xbf,0xe7,0x6f,0x65,0xfb,
    0xd5,0x1a,0xd2,0xf1,0x48,0x98,0xb9,0x5b,
};

static const uint8_t VEC_ABC[48] = {
    0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,
    0xb5,0xa0,0x3d,0x69,0x9a,0xc6,0x50,0x07,
    0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,
    0x1a,0x8b,0x60,0x5a,0x43,0xff,0x5b,0xed,
    0x80,0x86,0x07,0x2b,0xa1,0xe7,0xcc,0x23,
    0x58,0xba,0xec,0xa1,0x34,0xc8,0x25,0xa7,
};

static int mem_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[SHA384-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[SHA384-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void sha384_test(void)
{
    uint8_t out[48];
    failures = 0;

    /* 1) 空字符串 */
    sha384("", 0, out);
    check("empty string", mem_eq(out, VEC_EMPTY, 48));

    /* 2) "abc" */
    sha384("abc", 3, out);
    check("abc", mem_eq(out, VEC_ABC, 48));

    /* 3) 流式 update 一致性 */
    {
        uint8_t buf[300];
        for (int i = 0; i < 300; ++i) buf[i] = (uint8_t)(i & 0xFF);

        uint8_t ref[48];
        sha384(buf, 300, ref);

        struct sha384_ctx ctx;
        sha384_init(&ctx);
        sha384_update(&ctx, buf, 1);
        sha384_update(&ctx, buf + 1, 126);
        sha384_update(&ctx, buf + 127, 173);
        uint8_t streamed[48];
        sha384_final(&ctx, streamed);
        check("streamed update matches one-shot",
              mem_eq(ref, streamed, 48));
    }

    /* 4) 块边界 111/112/113 */
    {
        uint8_t buf[128];
        for (int i = 0; i < 128; ++i) buf[i] = (uint8_t)(i + 1);

        uint8_t r1[48], r2[48], r3[48];
        sha384(buf, 111, r1);
        sha384(buf, 112, r2);
        sha384(buf, 113, r3);

        /* 三次结果必须互不相同 */
        check("boundary 111 != 112", !mem_eq(r1, r2, 48));
        check("boundary 112 != 113", !mem_eq(r2, r3, 48));
    }

    /* 5) 128 字节（正好一整块）与 129 字节（一块零一字节）*/
    {
        uint8_t buf[200];
        for (int i = 0; i < 200; ++i) buf[i] = (uint8_t)i;

        uint8_t r128[48], r129[48];
        sha384(buf, 128, r128);
        sha384(buf, 129, r129);
        check("128 vs 129 differ", !mem_eq(r128, r129, 48));
    }

    if (failures == 0) {
        serial_printf("[SHA384-TEST] selftest OK (5 cases)\n");
    } else {
        serial_printf("[SHA384-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}
/*===OmniBridgeOs/kernel/arch/x64/sha384_test.c 结束===*/