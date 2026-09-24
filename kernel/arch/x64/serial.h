#ifndef OMNIBRIDGE_SERIAL_H
#define OMNIBRIDGE_SERIAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_printf(const char *fmt, ...);

int ob_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ob_snprintf(char *buf, size_t size, const char *fmt, ...);
char serial_getc_nonblock(void);
void serial_enable_rx_irq(void);
#endif