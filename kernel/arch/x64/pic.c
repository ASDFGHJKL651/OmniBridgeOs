#include "pic.h"
#include "serial.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01

#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PIT_FREQ   1193182u   /* 8254 输入时钟频率 */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ __volatile__("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_wait(void)
{
    /* 写入未使用的端口 0x80 会耗费约 1 us，作为 PIC 初始化间隔 */
    outb(0x80, 0);
}

void pic_init(void)
{
    /* ICW1：边沿触发 + 需要 ICW4 */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4);  io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4);  io_wait();

    /* ICW2：中断向量偏移 */
    outb(PIC1_DATA, 0x20);  io_wait();       /* 主片 -> 32..39 */
    outb(PIC2_DATA, 0x28);  io_wait();       /* 从片 -> 40..47 */

    /* ICW3：级联 */
    outb(PIC1_DATA, 0x04);  io_wait();       /* 主片 IRQ2 接从片 */
    outb(PIC2_DATA, 0x02);  io_wait();       /* 从片级联到 IRQ2 */

    /* ICW4：8086 模式 */
    outb(PIC1_DATA, ICW4_8086);  io_wait();
    outb(PIC2_DATA, ICW4_8086);  io_wait();

    /* 初始屏蔽全部 IRQ */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    serial_printf("[PIC] remapped: master=0x20..0x27, slave=0xA0..0xA3, offsets 32/40\n");
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);   /* 从片 EOI */
    }
    outb(PIC1_CMD, 0x20);       /* 主片 EOI */
}

void pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit   = (uint8_t)(1u << (irq & 7));
    outb(port, (uint8_t)(inb(port) | bit));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit   = (uint8_t)(1u << (irq & 7));
    outb(port, (uint8_t)(inb(port) & (uint8_t)~bit));
}

void pit_init(uint32_t hz)
{
    if (hz == 0) hz = 100;
    uint32_t divisor = PIT_FREQ / hz;
    if (divisor == 0)        divisor = 1;
    if (divisor > 0xFFFF)    divisor = 0xFFFF;

    /* 通道 0，访问 lo/hi，方式 3（方波），二进制 */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    serial_printf("[PIT] initialized at %u Hz (divisor=%u)\n",
                  (unsigned)hz, (unsigned)divisor);
}