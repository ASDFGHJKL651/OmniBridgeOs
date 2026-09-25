/*===OmniBridgeOs/kernel/arch/x64/mem.c===*/
/*
 * freestanding 内存/字符串函数。
 *
 * 存在理由（人工必须审查）：
 *   - 内核以 -ffreestanding 编译，clang 不会内联展开大块结构体赋值、
 *     结构体拷贝或数组初始化，而是生成对 memcpy/memset/memmove/memcmp
 *     的调用。
 *   - 本项目没有链接 libc，必须自行提供这些符号。
 *
 * 约定：
 *   - 所有函数符合 System V / MS ABI 的 C 调用约定。
 *   - 不做对齐假设；以字节为单位操作。
 *   - memmove 必须支持重叠区域。
 *   - 返回值语义严格遵循 C 标准。
 *
 * 性能说明：
 *   - 这里给出的是字节循环实现，简洁且无 UB。
 *   - 编译器可能在本文件内部将这些循环再优化为更宽的访问；
 *     但由于我们禁用了 SSE/MMX，实际生成的是 GP 寄存器版本，
 *     对内核启动阶段可接受。
 *   - 后续步骤如需性能优化，可替换为 8 字节/次的对齐快路径，
 *     但必须保证行为等价。
 */

#include <stddef.h>
#include <stdint.h>

/* ---------- memcpy ---------- */
void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    /* 简单的 8 字节快路径：两侧同对齐时一次拷贝 8 字节 */
    if (((uintptr_t)d & 7u) == 0 && ((uintptr_t)s & 7u) == 0) {
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }

    while (n--) *d++ = *s++;
    return dst;
}

/* ---------- memmove ---------- */
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0) return dst;

    if (d < s) {
        /* 正向拷贝是安全的 */
        while (n--) *d++ = *s++;
    } else {
        /* 从尾部反向拷贝，避免覆盖未读取的源数据 */
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

/* ---------- memset ---------- */
void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t v = (uint8_t)c;

    /* 8 字节快路径 */
    if (((uintptr_t)d & 7u) == 0) {
        uint64_t v64 = (uint64_t)v;
        v64 |= v64 << 8;
        v64 |= v64 << 16;
        v64 |= v64 << 32;

        while (n >= 8) {
            *(uint64_t *)d = v64;
            d += 8;
            n -= 8;
        }
    }

    while (n--) *d++ = v;
    return dst;
}

/* ---------- memcmp ---------- */
int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;

    while (n--) {
        if (*pa != *pb) {
            return (int)(*pa) - (int)(*pb);
        }
        ++pa;
        ++pb;
    }
    return 0;
}

/* ---------- memchr ---------- */
void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    uint8_t v = (uint8_t)c;
    while (n--) {
        if (*p == v) return (void *)p;
        ++p;
    }
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/mem.c 结束===*/