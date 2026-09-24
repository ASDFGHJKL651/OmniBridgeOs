#ifndef OMNIBRIDGE_PIC_H
#define OMNIBRIDGE_PIC_H

#include <stdint.h>

/*
 * 8259A 双片 PIC：
 *   主片端口 0x20/0x21，向量偏移 0x20（32..39）
 *   从片端口 0xA0/0xA1，向量偏移 0x28（40..47）
 */
void pic_init(void);

/* 发送 EOI（End Of Interrupt）。irq 为 0..15（IRQ 号，不是向量号）。 */
void pic_send_eoi(uint8_t irq);

/* 屏蔽指定 IRQ（irq 为 0..15） */
void pic_mask(uint8_t irq);

/* 解除屏蔽指定 IRQ */
void pic_unmask(uint8_t irq);

/*
 * 初始化 PIT（8254），频率 hz。
 * 使用通道 0（IRQ0），方式 3（方波），二进制计数。
 * hz 推荐 100。
 */
void pit_init(uint32_t hz);

#endif