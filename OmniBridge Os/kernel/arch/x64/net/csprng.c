/*===OmniBridgeOs/kernel/arch/x64/net/csprng.c===*/
#include "csprng.h"
#include "serial.h"

static int      g_hw    = 0;
static int      g_inited = 0;
static uint64_t g_counter = 0;

static inline uint64_t rdtsc_(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int rdrand64_(uint64_t *out)
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

static void cpuid_(uint32_t leaf, uint32_t sub,
                   uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(sub));
}

void csprng_init(void)
{
    if (g_inited) return;
    uint32_t a, b, c, d;
    cpuid_(1, 0, &a, &b, &c, &d);
    g_hw = (c >> 30) & 1;
    g_counter = rdtsc_() ^ 0xA5A5A5A5A5A5A5A5ULL;
    g_inited = 1;
    if (g_hw) {
        serial_printf("[CSPRNG] init: RDRAND\n");
    } else {
        serial_printf("[CSPRNG] WARN: CSPRNG 降级，非密码学安全\n");
    }
}

static uint64_t mix64_(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int csprng_bytes(void *out, uint64_t len)
{
    if (!g_inited) csprng_init();
    if (!out) return -1;

    uint8_t *p = (uint8_t *)out;
    uint64_t remaining = len;

    if (g_hw) {
        while (remaining >= 8) {
            uint64_t v;
            if (!rdrand64_(&v)) return -1;
            for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
            p += 8; remaining -= 8;
        }
        if (remaining) {
            uint64_t v;
            if (!rdrand64_(&v)) return -1;
            for (uint64_t i = 0; i < remaining; ++i)
                p[i] = (uint8_t)(v >> (8 * i));
        }
        return 0;
    }

    while (remaining) {
        g_counter = mix64_(g_counter + rdtsc_());
        uint64_t v = mix64_(g_counter);
        uint64_t take = remaining < 8 ? remaining : 8;
        for (uint64_t i = 0; i < take; ++i) p[i] = (uint8_t)(v >> (8 * i));
        p += take; remaining -= take;
    }
    return 0;
}

uint32_t csprng_u32(void)
{
    uint32_t v;
    if (csprng_bytes(&v, 4) != 0) return 0;
    return v;
}

uint16_t csprng_u16(void)
{
    uint16_t v;
    if (csprng_bytes(&v, 2) != 0) return 0;
    return v;
}

int csprng_is_hw(void) { return g_hw; }
/*===OmniBridgeOs/kernel/arch/x64/net/csprng.c 结束===*/