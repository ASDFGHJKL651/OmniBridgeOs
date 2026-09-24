/* kernel/arch/x64/rng.h
 * 硬件随机数生成器抽象层。
 *
 * 关键约束（人工必须审查）：
 *   - 首选 RDRAND（CPUID.01H:ECX[30]）。
 *   - 若 RDRAND 不可用，退化为基于 RDTSC + LAPIC_TIMER_CUR 的熵池。
 *   - QEMU TCG 模式下 RDRAND 可能被模拟为恒定值；人工须在 QEMU 中
 *     实测 rng_bytes() 连续两次输出不同。
 *   - rng_bytes() 失败时 ita_sign_init() 必须标记子系统不可用，而非崩溃。
 */
#ifndef OMNIBRIDGE_RNG_H
#define OMNIBRIDGE_RNG_H

#include <stdint.h>

void rng_init(void);

/* 0 成功，-1 失败 */
int rng_bytes(void *out, uint64_t len);

#endif /* OMNIBRIDGE_RNG_H */