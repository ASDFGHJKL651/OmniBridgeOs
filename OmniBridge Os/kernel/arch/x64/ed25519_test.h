/*===OmniBridgeOs/kernel/arch/x64/ed25519_test.h===*/
#ifndef OMNIBRIDGE_ED25519_TEST_H
#define OMNIBRIDGE_ED25519_TEST_H

/*
 * Ed25519 自检入口（第 14 步阶段 14b）。
 *
 * 使用 RFC 8032 §7.1 的官方已知答案测试（KAT）：
 *   TEST 1（空消息）
 *   TEST 2（1 字节 0x72）
 *   TEST 3（2 字节 0xaf 0x82）
 *
 * 校验项：
 *   - 从 seed 派生的公钥与 RFC 8032 一致；
 *   - 签名与 RFC 8032 一致（在签名实现完整时）；
 *   - 篡改签名字节 → 验证失败；
 *   - msg == NULL && msg_len > 0 → 拒绝。
 *
 * 由 main.c 在 sha512_test() 之后调用。
 */
void ed25519_test(void);

#endif /* OMNIBRIDGE_ED25519_TEST_H */
/*===OmniBridgeOs/kernel/arch/x64/ed25519_test.h 结束===*/