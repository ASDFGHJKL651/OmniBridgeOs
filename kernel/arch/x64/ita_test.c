/*===OmniBridgeOs/kernel/arch/x64/ita_test.c===*/
#include "ita_test.h"
#include "ita.h"
#include "sha384.h"
#include "serial.h"

/*
 * 注意：不测试 panic 路径。
 * 依赖：ita_fixed_hashes.h 中的占位哈希 = SHA-384("")，因此用空 buffer
 *       可验证"匹配"分支。
 */

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[ITA-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[ITA-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void ita_test(void)
{
    failures = 0;

    /* 1) 初始化 */
    ita_init();
    check("whitelist count > 0", ita_fixed_whitelist_count() > 0);

    /* 2) SHA-384 与 FIPS 向量一致（重复验证 sha384_test 已覆盖，这里
     *    确认 sha384() 用于 ITA 的一致性）。 */
    uint8_t d[48];
    sha384("", 0, d);
    int all_zero = 1;
    for (int i = 0; i < 48; ++i) if (d[i]) { all_zero = 0; break; }
    check("sha384 empty is not all-zero", !all_zero);

    /* 3) 不存在的路径 */
    int rc = ita_verify_fixed("/nonexistent/path.obr", "", 0);
    check("unknown path returns -ENOENT", rc == -2);

    /* 4) 已知路径 + 匹配内容（占位哈希是 SHA-384("")） */
    rc = ita_verify_fixed("/system/critical/init.obr", "", 0);
    check("known path with empty content returns 0", rc == 0);

    /* 5) 已知路径 + 篡改内容 */
    const char *tampered = "tampered!";
    rc = ita_verify_fixed("/system/critical/init.obr", tampered, 9);
    check("known path with tampered content returns -EACCES", rc == -13);

    if (failures == 0) {
        serial_printf("[ITA-TEST] selftest OK\n");
    } else {
        serial_printf("[ITA-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}
/*===OmniBridgeOs/kernel/arch/x64/ita_test.c 结束===*/