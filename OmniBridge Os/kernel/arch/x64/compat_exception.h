/* kernel/arch/x64/compat_exception.h
 * 兼容层异常转换入口（第 18 步）。
 */
#ifndef OMNIBRIDGE_COMPAT_EXCEPTION_H
#define OMNIBRIDGE_COMPAT_EXCEPTION_H

#include <stdint.h>
#include "idt.h"
#include "task.h"

/* 由 exception_handler 分派。
 * 返回 0 = 未处理（调用方走原 panic 路径）；
 * 返回 1 = 已处理（调用方必须 task_exit(-1)）。 */
int compat_exception_handle(struct regs *r, struct task_t *cur);

#endif /* OMNIBRIDGE_COMPAT_EXCEPTION_H */