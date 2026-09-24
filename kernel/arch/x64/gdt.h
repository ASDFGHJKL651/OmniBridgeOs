#ifndef OMNIBRIDGE_GDT_H
#define OMNIBRIDGE_GDT_H

#include <stdint.h>

/*
 * GDT 布局（共 7 项）：
 *   0  null
 *   1  kernel code  (0x08)
 *   2  kernel data  (0x10)
 *   3  user code    (0x1B / 0x18 | RPL3)
 *   4  user data    (0x23 / 0x20 | RPL3)
 *   5  TSS low      (0x28)
 *   6  TSS high
 */
void gdt_init(void);

#endif