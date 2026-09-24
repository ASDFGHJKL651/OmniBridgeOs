/*===OmniBridgeOs/usr/lib/oblibc/stdlib.c===*/
#include "../../include/ob/stdlib.h"
#include "../../include/ob/ob.h"
#include "../../include/ob/unistd.h"
#include "../../include/ob/errno.h"
#include <stdint.h>

/* 简化 malloc：使用 mmap 类系统调用 SYS_OB_Brk 向上扩展堆。
 * 每个分配前置 16 字节头，记录大小和 magic。 */

#define HEAP_MAGIC 0xB16B00B5u

struct heap_hdr {
    uint32_t magic;
    uint32_t size;         /* 用户可见大小 */
    uint64_t _pad;
};

extern void *ob_brk(uint64_t new_brk);

void *ob_brk(uint64_t new_brk)
{
    int64_t r = ob_syscall1(SYS_OB_Brk, new_brk);
    if (r < 0) { errno = ENOMEM; return 0; }
    return (void *)(uintptr_t)r;
}

static void *heap_cur  = 0;
static void *heap_end  = 0;

void *malloc(size_t size)
{
    if (size == 0) return 0;
    if (!heap_cur) {
        heap_cur = ob_brk(0);
        heap_end = heap_cur;
    }
    size_t need = ((size + 15) & ~15ULL) + sizeof(struct heap_hdr);
    if ((char *)heap_cur + need > (char *)heap_end) {
        uint64_t want = (uint64_t)(uintptr_t)heap_cur + need + 0x100000;
        void *r = ob_brk(want);
        if (!r) return 0;
        heap_end = (char *)r + ((uint64_t)(uintptr_t)r -
                                (uint64_t)(uintptr_t)heap_cur);
        /* 若 brk 返回的是新位置 */
        if ((uint64_t)(uintptr_t)r > (uint64_t)(uintptr_t)heap_cur) {
            heap_end = r;
        } else {
            heap_end = (char *)heap_cur + need;
        }
    }
    struct heap_hdr *h = (struct heap_hdr *)heap_cur;
    h->magic = HEAP_MAGIC;
    h->size  = (uint32_t)size;
    void *user = (char *)h + sizeof(*h);
    heap_cur = (char *)user + ((size + 15) & ~15ULL);
    return user;
}

void free(void *p)
{
    /* 简化：本步不回收（brk-only 分配器） */
    (void)p;
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = malloc(total);
    if (!p) return 0;
    unsigned char *b = p;
    for (size_t i = 0; i < total; ++i) b[i] = 0;
    return p;
}

void *realloc(void *p, size_t size)
{
    if (!p) return malloc(size);
    if (size == 0) { free(p); return 0; }
    struct heap_hdr *h = (struct heap_hdr *)((char *)p - sizeof(*h));
    if (h->magic != HEAP_MAGIC) return 0;
    if (size <= h->size) return p;
    void *np = malloc(size);
    if (!np) return 0;
    unsigned char *s = p, *d = np;
    for (size_t i = 0; i < h->size; ++i) d[i] = s[i];
    free(p);
    return np;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    if (*s == '-') { sign = -1; ++s; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    return sign * v;
}

long atol(const char *s) { return (long)atoi(s); }

char *itoa(int v, char *buf, int base)
{
    char tmp[32];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v) {
        int d = v % base;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return buf;
}

void exit(int code) { _exit(code); for (;;) {} }
void abort(void)    { _exit(134); for (;;) {} }
/*===OmniBridgeOs/usr/lib/oblibc/stdlib.c 结束===*/