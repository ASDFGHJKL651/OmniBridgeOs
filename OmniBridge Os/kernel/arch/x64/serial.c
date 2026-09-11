#include "serial.h"

#define COM1_PORT 0x3F8

extern void serial_outb(uint16_t port, uint8_t value);
extern uint8_t serial_inb(uint16_t port);

/*
 * HUMAN REVIEW REQUIRED:
 *   - COM1 基址 0x3F8。
 *   - 初始化序列假设标准 16550 UART。
 *   - 轮询发送，不依赖中断。
 */
void serial_init(void)
{
    serial_outb(COM1_PORT + 1, 0x00); /* 禁用中断 */
    serial_outb(COM1_PORT + 3, 0x80); /* 启用 DLAB */
    serial_outb(COM1_PORT + 0, 0x03); /* 除数低字节 */
    serial_outb(COM1_PORT + 1, 0x00); /* 除数高字节 */
    serial_outb(COM1_PORT + 3, 0x03); /* 8N1 */
    serial_outb(COM1_PORT + 2, 0xC7); /* FIFO */
    serial_outb(COM1_PORT + 4, 0x0B); /* IRQ、RTS/DSR */
}

static int serial_tx_empty(void)
{
    return (serial_inb(COM1_PORT + 5) & 0x20) != 0;
}

void serial_putc(char c)
{
    while (!serial_tx_empty()) {
        /* 忙等待 */
    }
    serial_outb(COM1_PORT, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

void serial_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    int n = ob_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }

    serial_puts(buf);
}