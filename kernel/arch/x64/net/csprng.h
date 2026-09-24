/*===OmniBridgeOs/kernel/arch/x64/net/csprng.h===*/
#ifndef OMNIBRIDGE_NET_CSPRNG_H
#define OMNIBRIDGE_NET_CSPRNG_H

#include <stdint.h>

/*
 * CSPRNG 封装（第 18A 步）。
 *
 * 优先 RDRAND；若不可用则混合 TSC 与序列号，打印 WARN：
 *   "CSPRNG 降级，非密码学安全"
 *
 * 用于：TCP ISN、端口随机、DNS 事务 ID、DHCP 事务 ID。
 */
void     csprng_init(void);
int      csprng_bytes(void *out, uint64_t len);
uint32_t csprng_u32(void);
uint16_t csprng_u16(void);

/* 是否使用硬件 RDRAND */
int      csprng_is_hw(void);

#endif /* OMNIBRIDGE_NET_CSPRNG_H */
/*===OmniBridgeOs/kernel/arch/x64/net/csprng.h 结束===*/