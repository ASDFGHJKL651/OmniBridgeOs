/* kernel/arch/x64/rng.c
 * RNG 抽象层实现。
 */
#include "rng.h"
#include "serial.h"

static int g_has_rdrand = 0;
static int g_inited     = 0;

static inline void cpuid_count(uint32_t leaf, uint32_t subleaf,
                               uint32_t *a, uint32_t *b,
                               uint32_t *c, uint32_t *d)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(subleaf));
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* RDRAND 尝试；成功返回 1，失败返回 0 */
static int rdrand64(uint64_t *out)
{
    uint8_t ok;
    uint64_t v;
    __asm__ __volatile__(
        "rdrand %0\n\t"
        "setc   %1\n\t"
        : "=r"(v), "=rm"(ok)
        :
        : "cc");
    if (!ok) return 0;
    *out = v;
    return 1;
}

void rng_init(void)
{
    if (g_inited) return;
    uint32_t a, b, c, d;
    cpuid_count(1, 0, &a, &b, &c, &d);
    g_has_rdrand = (c >> 30) & 1;
    g_inited = 1;
    serial_printf("[RNG] init: rdrand=%s\n",
                  g_has_rdrand ? "yes" : "no (fallback to TSC)");
}

/* 简单 mix 函数（splitmix64 风格） */
static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int rng_bytes(void *out, uint64_t len)
{
    if (!g_inited) rng_init();
    if (!out) return -1;

    uint8_t *p = (uint8_t *)out;
    uint64_t remaining = len;

    if (g_has_rdrand) {
        while (remaining >= 8) {
            uint64_t v;
            if (!rdrand64(&v)) return -1;
            for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
            p += 8;
            remaining -= 8;
        }
        if (remaining > 0) {
            uint64_t v;
            if (!rdrand64(&v)) return -1;
            for (uint64_t i = 0; i < remaining; ++i)
                p[i] = (uint8_t)(v >> (8 * i));
        }
        return 0;
    }

    /* 回退：基于 TSC 的熵池。非密码学安全，仅用于无 RDRAND 时 */
    uint64_t state = rdtsc() ^ 0xA5A5A5A5A5A5A5A5ULL;
    while (remaining > 0) {
        state = mix64(state);
        uint64_t v = mix64(state ^ rdtsc());
        uint64_t take = (remaining < 8) ? remaining : 8;
        for (uint64_t i = 0; i < take; ++i)
            p[i] = (uint8_t)(v >> (8 * i));
        p += take;
        remaining -= take;
    }
    return 0;
}