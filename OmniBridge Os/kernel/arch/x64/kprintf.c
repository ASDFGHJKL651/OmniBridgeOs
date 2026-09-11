#include "serial.h"

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} out_t;

static void out_char(out_t *o, char c)
{
    if (o->size > 0 && o->pos + 1 < o->size) {
        o->buf[o->pos] = c;
    }
    o->pos++;
}

static void out_str(out_t *o, const char *s)
{
    while (*s) {
        out_char(o, *s++);
    }
}

static void out_uint(out_t *o, uint64_t v, unsigned base, int upper, int width, char pad)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        tmp[i++] = '0';
    }

    while (v) {
        tmp[i++] = digits[v % base];
        v /= base;
    }

    while (i < width) {
        tmp[i++] = pad;
    }

    while (i > 0) {
        out_char(o, tmp[--i]);
    }
}

static void out_int(out_t *o, int64_t v, int width, char pad)
{
    if (v < 0) {
        out_char(o, '-');
        out_uint(o, (uint64_t)(-v), 10, 0, width > 0 ? width - 1 : 0, pad);
    } else {
        out_uint(o, (uint64_t)v, 10, 0, width, pad);
    }
}

int ob_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_t o = { buf, size, 0 };

    while (*fmt) {
        if (*fmt != '%') {
            out_char(&o, *fmt++);
            continue;
        }

        fmt++;

        int width = 0;
        char pad = ' ';

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }

        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                is_long = 2;
                fmt++;
            }
        }

        switch (*fmt) {
        case 's':
            out_str(&o, va_arg(ap, const char *));
            break;
        case 'c':
            out_char(&o, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i':
            if (is_long == 2) {
                out_int(&o, va_arg(ap, long long), width, pad);
            } else if (is_long == 1) {
                out_int(&o, va_arg(ap, long), width, pad);
            } else {
                out_int(&o, va_arg(ap, int), width, pad);
            }
            break;
        case 'u':
            if (is_long == 2) {
                out_uint(&o, va_arg(ap, unsigned long long), 10, 0, width, pad);
            } else if (is_long == 1) {
                out_uint(&o, va_arg(ap, unsigned long), 10, 0, width, pad);
            } else {
                out_uint(&o, va_arg(ap, unsigned int), 10, 0, width, pad);
            }
            break;
        case 'x':
        case 'X':
            if (is_long == 2) {
                out_uint(&o, va_arg(ap, unsigned long long), 16, (*fmt == 'X'), width, pad);
            } else if (is_long == 1) {
                out_uint(&o, va_arg(ap, unsigned long), 16, (*fmt == 'X'), width, pad);
            } else {
                out_uint(&o, va_arg(ap, unsigned int), 16, (*fmt == 'X'), width, pad);
            }
            break;
        case 'p':
            out_str(&o, "0x");
            out_uint(&o, (uint64_t)(uintptr_t)va_arg(ap, void *), 16, 0, width, pad);
            break;
        case '%':
            out_char(&o, '%');
            break;
        default:
            out_char(&o, '%');
            if (*fmt) {
                out_char(&o, *fmt);
            }
            break;
        }

        if (*fmt) {
            fmt++;
        }
    }

    if (size > 0) {
        size_t term = (o.pos < size) ? o.pos : size - 1;
        buf[term] = '\0';
    }

    return (int)o.pos;
}

int ob_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = ob_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}