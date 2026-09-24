#include "printk.h"
#include "serial.h"
#include <stdarg.h>

void printk(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = ob_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    serial_puts(buf);
}