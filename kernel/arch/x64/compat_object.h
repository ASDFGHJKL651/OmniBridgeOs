/* kernel/arch/x64/compat_object.h
 * 兼容层对象模拟层：信号/异常分发（第 18 步）。
 */
#ifndef OMNIBRIDGE_COMPAT_OBJECT_H
#define OMNIBRIDGE_COMPAT_OBJECT_H

#include <stdint.h>
#include "task.h"

#define OB_SIG_NONE  0
#define OB_SIGFPE    8
#define OB_SIGKILL   9
#define OB_SIGSEGV   11
#define OB_SIGILL    4
#define OB_SIGBUS    7
#define OB_SIGTERM   15

struct compat_signal_info {
    uint32_t signo;
    uint32_t code;
    uint64_t fault_addr;
    uint64_t rip;
};

void compat_object_init(void);

/* 根据异常向量映射为 OB 信号。只做映射，不终止进程。
 * 成功返回 0。 */
int compat_signal_translate(uint64_t vector, uint64_t error,
                            uint64_t cr2, uint64_t rip,
                            struct compat_signal_info *out);

/* 记录审计并打印分发信息。返回 0 表示已记录。 */
int compat_signal_dispatch(struct task_t *cur,
                           const struct compat_signal_info *info);

void compat_object_dump(void);

#endif /* OMNIBRIDGE_COMPAT_OBJECT_H */