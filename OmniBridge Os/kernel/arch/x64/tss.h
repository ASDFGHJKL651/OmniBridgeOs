#ifndef OMNIBRIDGE_TSS_H
#define OMNIBRIDGE_TSS_H

#include <stdint.h>

/*
 * x86_64 TSS（Task State Segment）。
 * 64 位模式下 TSS 不再用于任务切换，仅用于：
 *   - 提供 RSP0：中断/异常/syscall 从用户态进入内核态时的内核栈指针
 *   - 提供 ISTn：紧急异常的栈
 *   - 提供 I/O 位图基址
 *
 * 结构严格 packed，字段布局必须符合 Intel SDM Vol 3A 7.7 节。
 * 总大小 = 4 + 8*3 + 8 + 8*7 + 8 + 2 + 2 = 104 字节。
 */
struct tss_entry {
    uint32_t reserved0;      /* 0x00 保留 */
    uint64_t rsp0;           /* 0x04 Ring0 栈顶 */
    uint64_t rsp1;           /* 0x0C 未用 */
    uint64_t rsp2;           /* 0x14 未用 */
    uint64_t reserved1;      /* 0x1C 保留 */
    uint64_t ist[7];         /* 0x24 IST1..IST7 */
    uint64_t reserved2;      /* 0x5C 保留 */
    uint16_t reserved3;      /* 0x64 保留 */
    uint16_t iomap_base;     /* 0x66 I/O 位图基址偏移 */
} __attribute__((packed));

/* 返回全局 TSS 地址（供 gdt.c 编码 TSS 描述符使用） */
struct tss_entry *tss_get(void);

/* 设置 TSS.rsp0 */
void tss_set_rsp0(uint64_t rsp0);

/* 初始化 TSS 并执行 ltr 0x28 */
void tss_init(uint64_t rsp0);

#endif