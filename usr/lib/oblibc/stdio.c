/*===OmniBridgeOs/usr/lib/oblibc/stdio.c===*/
#include "../../include/ob/stdio.h"
#include "../../include/ob/unistd.h"
#include "../../include/ob/stdlib.h"      /* ★ 18C：malloc/free */
#include "../../include/ob/ob.h"
#include "../../include/ob/string.h"
#include <stdint.h>

struct ob_FILE {
    int      fd;
    int      owned;
    uint32_t pos;
};

static ob_FILE g_stdin  = { 0, 0, 0 };
static ob_FILE g_stdout = { 1, 0, 0 };
static ob_FILE g_stderr = { 2, 0, 0 };

ob_FILE *stdin  = &g_stdin;
ob_FILE *stdout = &g_stdout;
ob_FILE *stderr = &g_stderr;

static void out_str(const char *s, size_t n)
{
    (void)write(1, s, n);
}

int putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s)
{
    size_t n = strlen(s);
    write(1, s, n);
    putchar('\n');
    return 0;
}

static int fmt_int(char *buf, size_t cap, long long v, int base,
                   int upper, int width, int zero)
{
    char tmp[32];
    int i = 0, neg = 0;
    if (v < 0 && base == 10) { neg = 1; v = -v; }
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = digs[v % base]; v /= base; }
    if (neg) tmp[i++] = '-';

    int pad = width - i;
    int w = 0;
    if (cap == 0) return pad > 0 ? pad + i : i;
    while (pad-- > 0 && (size_t)w + 1 < cap)
        buf[w++] = zero ? '0' : ' ';
    while (i > 0 && (size_t)w + 1 < cap)
        buf[w++] = tmp[--i];
    if ((size_t)w < cap) buf[w] = '\0';
    return w;
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    size_t w = 0;
    #define PUT(c) do { if (w + 1 < cap) buf[w] = (c); ++w; } while (0)

    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        fmt++;
        int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0'); fmt++;
        }
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            long long v = is_long ? va_arg(ap, long long)
                                  : (long long)va_arg(ap, int);
            char tmp[40];
            int n = fmt_int(tmp, sizeof(tmp), v, 10, 0, width, zero);
            for (int i = 0; i < n; ++i) PUT(tmp[i]);
            break; }
        case 'u': {
            unsigned long long v = is_long
                ? va_arg(ap, unsigned long long)
                : (unsigned long long)va_arg(ap, unsigned);
            char tmp[40];
            int n = fmt_int(tmp, sizeof(tmp), (long long)v, 10, 0, width, zero);
            for (int i = 0; i < n; ++i) PUT(tmp[i]);
            break; }
        case 'x': case 'X': {
            unsigned long long v = is_long
                ? va_arg(ap, unsigned long long)
                : (unsigned long long)va_arg(ap, unsigned);
            char tmp[40];
            int n = fmt_int(tmp, sizeof(tmp), (long long)v, 16,
                            *fmt == 'X', width, zero);
            for (int i = 0; i < n; ++i) PUT(tmp[i]);
            break; }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            PUT('0'); PUT('x');
            char tmp[40];
            int n = fmt_int(tmp, sizeof(tmp), (long long)v, 16, 0, 0, 0);
            for (int i = 0; i < n; ++i) PUT(tmp[i]);
            break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) PUT(*s++);
            break; }
        case 'c': {
            int c = va_arg(ap, int);
            PUT((char)c);
            break; }
        case '%': PUT('%'); break;
        default:
            PUT('%'); if (*fmt) PUT(*fmt); break;
        }
        if (*fmt) fmt++;
    }
    if (cap > 0) buf[w < cap ? w : cap - 1] = '\0';
    return (int)w;
    #undef PUT
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out_str(buf, (size_t)n);
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) out_str(buf, (size_t)n);
    return n;
}

int fprintf(ob_FILE *f, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && f) write(f->fd, buf, (size_t)n);
    return n;
}

int getchar(void)
{
    char c;
    if (read(0, &c, 1) <= 0) return EOF;
    return (unsigned char)c;
}

ob_FILE *fopen(const char *path, const char *mode)
{
    int flags = 0;
    if (mode[0] == 'r') flags = 1;
    else if (mode[0] == 'w') flags = 2 | 0x10 | 0x40;
    else if (mode[0] == 'a') flags = 2 | 0x10 | 0x80;
    else return 0;
    int fd = open(path, flags, 0644);
    if (fd < 0) return 0;
    ob_FILE *f = (ob_FILE *)malloc(sizeof(*f));
    if (!f) { close(fd); return 0; }
    f->fd = fd; f->owned = 1; f->pos = 0;
    return f;
}

int fclose(ob_FILE *f)
{
    if (!f) return -1;
    int r = 0;
    if (f->owned) r = close(f->fd);
    free(f);
    return r;
}

size_t fread(void *buf, size_t sz, size_t n, ob_FILE *f)
{
    if (!f || sz == 0) return 0;
    int64_t r = read(f->fd, buf, sz * n);
    if (r <= 0) return 0;
    return (size_t)r / sz;
}

size_t fwrite(const void *buf, size_t sz, size_t n, ob_FILE *f)
{
    if (!f || sz == 0) return 0;
    int64_t r = write(f->fd, buf, sz * n);
    if (r <= 0) return 0;
    return (size_t)r / sz;
}

int fseek(ob_FILE *f, long off, int whence)
{
    (void)off; (void)whence;
    if (!f) return -1;
    f->pos = 0;
    return 0;
}

long ftell(ob_FILE *f) { return f ? (long)f->pos : -1; }
int  fflush(ob_FILE *f) { (void)f; return 0; }
/*===OmniBridgeOs/usr/lib/oblibc/stdio.c 结束===*/