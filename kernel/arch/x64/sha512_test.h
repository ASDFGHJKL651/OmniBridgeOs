/*===OmniBridgeOs/kernel/arch/x64/sha512_test.h===*/
#ifndef OMNIBRIDGE_SHA512_TEST_H
#define OMNIBRIDGE_SHA512_TEST_H

/*
 * SHA-512 自检入口（第 14 步阶段 14b）。
 *
 * 使用 FIPS 180-4 官方向量：
 *   SHA-512("")    = cf83e1357eefb8bd...
 *   SHA-512("abc") = ddaf35a193617aba...
 *
 * 并验证流式 update 与一次性调用一致。
 *
 * 由 main.c 在 ita_test() 之后调用。
 */
void sha512_test(void);

#endif /* OMNIBRIDGE_SHA512_TEST_H */
/*===OmniBridgeOs/kernel/arch/x64/sha512_test.h 结束===*/