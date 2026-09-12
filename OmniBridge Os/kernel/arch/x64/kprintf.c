#include "serial.h"

/*
 * ob_vsnprintf —— freestanding 环境的精简 printf 实现。
 *
 * 支持的格式：
 *   %[-][0][width][l|ll]{d|i|u|x|X|p|s|c|%}
 *
 * 语义要点（与 C 标准一致）：
 *   - `-`：左对齐；`0`：用 '0' 填充（仅对整数有意义）。
 *   - `-` 与 `0` 同时出现时以 `-` 为准，填充字符退化为空格。
 *   - 宽度作用于「整个字段」，对 `%p` 而言仅作用于十六进制数字部分
 *     （"0x" 前缀不计入宽度），这是本实现的一处有意简化。
 *   - 不支持的格式字符按「原样输出」处理：'%' + 该字符。
 *     —— 此策略可避免解析器在未知格式上「吃掉」参数导致后续错位。
 *
 * 兼容性提示（人工必须审查）：
 *   本实现的解析器在遇到无法识别的格式时，**不会** va_arg 消耗参数。
 *   因此绝不要在未支持的格式（如 `%e`、`%g`、`%f`、`%n`、`%*`）上
 *   期望标准行为 —— 会错位。内核代码内部目前未使用这些格式。
 */

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
} out_t;

/* ---------- 底层输出 ---------- */

static void out_char(out_t *o, char c)
{
    if (o->size > 0 && o->pos + 1 < o->size) {
        o->buf[o->pos] = c;
    }
    o->pos++;
}

/* 输出 n 个相同的填充字符（n < 0 视为 0） */
static void out_pad(out_t *o, char pad, int n)
{
    while (n-- > 0) out_char(o, pad);
}

/* 输出裸字符串（无填充），供内部使用 */
static void out_str(out_t *o, const char *s)
{
    while (*s) out_char(o, *s++);
}

/* 输出字符串，支持最小宽度与左对齐。
 *   - 字符串长度 <  width 时用空格补齐到 width；
 *   - 字符串长度 >= width 时按原样输出。
 */
static void out_str_padded(out_t *o, const char *s, int width, int left_align)
{
    if (!s) s = "(null)";

    size_t len = 0;
    const char *p = s;
    while (*p++) ++len;

    if (width < 0) width = 0;
    int pad_n = (int)len < width ? width - (int)len : 0;

    if (!left_align) out_pad(o, ' ', pad_n);
    out_str(o, s);
    if (left_align)  out_pad(o, ' ', pad_n);
}

/* 输出单个字符，支持最小宽度与左对齐。填充统一用空格。 */
static void out_char_padded(out_t *o, char c, int width, int left_align)
{
    if (width < 1) width = 1;
    int pad_n = width - 1;

    if (!left_align) out_pad(o, ' ', pad_n);
    out_char(o, c);
    if (left_align)  out_pad(o, ' ', pad_n);
}

/* 无符号整数输出。
 *   v         : 数值
 *   base      : 10 或 16
 *   upper     : 十六进制是否用大写
 *   width     : 最小字段宽度（含填充）
 *   pad       : 填充字符（'0' 或 ' '）
 *   left_align: 1 左对齐；0 右对齐
 */
static void out_uint(out_t *o, uint64_t v, unsigned base, int upper,
                     int width, char pad, int left_align)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v) {
            tmp[i++] = digits[v % base];
            v /= base;
        }
    }
    int ndigits = i;
    int pad_n   = (ndigits < width) ? (width - ndigits) : 0;

    if (!left_align) out_pad(o, pad, pad_n);
    /* tmp 是最低位在前，逆序输出 */
    while (i > 0) out_char(o, tmp[--i]);
    if (left_align)  out_pad(o, ' ', pad_n);   /* 左对齐时填充总是空格 */
}

/* 有符号整数输出。
 *   处理 INT64_MIN 的溢出陷阱：用 (uint64_t)(-(v+1)) + 1 避免 UB。
 *   负号占一个字段宽度位置。
 */
static void out_int(out_t *o, int64_t v, int width, char pad, int left_align)
{
    if (v >= 0) {
        out_uint(o, (uint64_t)v, 10, 0, width, pad, left_align);
        return;
    }

    /* 处理负数：拆成 '-' 与绝对值两部分，以便统一控制宽度 */
    uint64_t abs_v = (uint64_t)(-(v + 1)) + 1u;   /* 对 INT64_MIN 也安全 */

    char tmp[32];
    int i = 0;
    {
        uint64_t t = abs_v;
        if (t == 0) tmp[i++] = '0';
        while (t) { tmp[i++] = (char)('0' + (t % 10)); t /= 10; }
    }
    int ndigits = i;
    int total   = 1 + ndigits;                    /* '-' + 数字位数 */
    int pad_n   = (total < width) ? (width - total) : 0;

    if (!left_align) out_pad(o, pad, pad_n);
    out_char(o, '-');
    while (i > 0) out_char(o, tmp[--i]);
    if (left_align)  out_pad(o, ' ', pad_n);      /* 左对齐时填充总是空格 */
}

/* ---------- 主入口 ---------- */

int ob_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    out_t o = { buf, size, 0 };

    while (*fmt) {
        if (*fmt != '%') {
            out_char(&o, *fmt++);
            continue;
        }

        fmt++;   /* 跳过 '%' */

        /* ---- 1) 解析 flags：任意顺序的 '-' 与 '0' ---- */
        int  left_align = 0;
        char pad        = ' ';
        for (;;) {
            if (*fmt == '-') { left_align = 1; fmt++; continue; }
            if (*fmt == '0') { pad        = '0'; fmt++; continue; }
            break;
        }
        /* 按 C 标准：'-' 与 '0' 同时出现时以 '-' 为准 */
        if (left_align) pad = ' ';

        /* ---- 2) 解析最小字段宽度 ---- */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* ---- 3) 解析长度修饰符 l / ll ---- */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') { is_long = 2; fmt++; }
        }

        /* ---- 4) 分发 ---- */
        switch (*fmt) {
        case 's':
            out_str_padded(&o, va_arg(ap, const char *), width, left_align);
            break;

        case 'c':
            out_char_padded(&o, (char)va_arg(ap, int), width, left_align);
            break;

        case 'd':
        case 'i':
            if (is_long == 2) {
                out_int(&o, va_arg(ap, long long), width, pad, left_align);
            } else if (is_long == 1) {
                out_int(&o, va_arg(ap, long), width, pad, left_align);
            } else {
                out_int(&o, va_arg(ap, int), width, pad, left_align);
            }
            break;

        case 'u':
            if (is_long == 2) {
                out_uint(&o, va_arg(ap, unsigned long long),
                         10, 0, width, pad, left_align);
            } else if (is_long == 1) {
                out_uint(&o, va_arg(ap, unsigned long),
                         10, 0, width, pad, left_align);
            } else {
                out_uint(&o, va_arg(ap, unsigned int),
                         10, 0, width, pad, left_align);
            }
            break;

        case 'x':
        case 'X':
            if (is_long == 2) {
                out_uint(&o, va_arg(ap, unsigned long long),
                         16, (*fmt == 'X'), width, pad, left_align);
            } else if (is_long == 1) {
                out_uint(&o, va_arg(ap, unsigned long),
                         16, (*fmt == 'X'), width, pad, left_align);
            } else {
                out_uint(&o, va_arg(ap, unsigned int),
                         16, (*fmt == 'X'), width, pad, left_align);
            }
            break;

        case 'p':
            /* 宽度只作用于十六进制数字部分（"0x" 前缀不计入） */
            out_str(&o, "0x");
            out_uint(&o, (uint64_t)(uintptr_t)va_arg(ap, void *),
                     16, 0, width, pad, left_align);
            break;

        case '%':
            out_char(&o, '%');
            break;

        default:
            /* 未支持的格式：原样输出 '%' 与当前字符。
             * 关键：不调用 va_arg，避免参数错位传播到后续格式。 */
            out_char(&o, '%');
            if (*fmt) out_char(&o, *fmt);
            break;
        }

        if (*fmt) fmt++;
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