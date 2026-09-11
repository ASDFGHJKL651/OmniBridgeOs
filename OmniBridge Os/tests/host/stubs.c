/* stubs.c —— 备选：让宿主也走真实 vsnprintf 路径 */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include "serial.h"



/* 内核串口在宿主不可用，这里只是占位，不会被 printk 调用 */
void serial_init(void)                       {}
void serial_putc(char c)                     { (void)c; }
void serial_puts(const char *s)              { fputs(s, stdout); }
void serial_printf(const char *fmt, ...)     { (void)fmt; }