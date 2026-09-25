#ifndef OMNIBRIDGE_PID_H
#define OMNIBRIDGE_PID_H

#include <stdint.h>

/*
 * PID 位图管理。
 *
 * 设计（人工必须审查）：
 *   - 支持 PID 0..65535，位图 8192 字节（65536 bit）。
 *   - pid_init 只置位 PID 0（引导/空闲线程）。
 *   - 1..99 的"保留"语义由 pid_is_reserved() 在分配/访问路径上统一施加：
 *       * alloc_pid() 拒绝 requested 落在 0..99；
 *       * free_pid() 拒绝释放 0..99；
 *       * check_pid_access() 拒绝非保留区 caller 访问保留区 target。
 *   - alloc_pid_kernel() 仅供内核引导路径使用，允许分配 1..99。
 *   - 自动分配扫描顺序：1000..65535 优先，耗尽后回退 100..999。
 *   - 单一自旋锁 g_pid_lock 保护所有位图操作；允许在 g_task_lock 内被调用，
 *     不允许在 pid 锁内获取 g_task_lock（锁序固定）。
 */

#define PID_BITMAP_BYTES   8192
#define PID_BITMAP_BITS    (PID_BITMAP_BYTES * 8)

/* 初始化 PID 子系统：清零位图并置位 PID 0。可多次调用（幂等）。 */
void pid_init(void);

/*
 * 分配 PID（用户态/常规路径）。
 *   requested == 0：自动分配（1000..65535 优先，回退 100..999）。
 *   requested != 0：指定分配。
 *
 * 返回：
 *   >= 0         : 分配成功的 PID
 *   OB_EINVAL    : requested 落在 0..99（保留区）或 > PID_MAX
 *   OB_EAGAIN    : requested 已占用，或自动分配无可用 PID
 */
int64_t alloc_pid(uint64_t requested);

/*
 * 释放 PID。
 *   返回  0  : 成功
 *   返回 -1  : pid == 0、落在保留区，或未分配
 */
int free_pid(uint64_t pid);

/* 返回 1 表示 pid 落在 0..99。 */
int pid_is_reserved(uint64_t pid);

/*
 * 内核启动服务专用：允许分配保留区 PID（1..99）。
 *
 * 与 alloc_pid() 的区别：
 *   - alloc_pid() 拒绝 requested 落在 0..99；
 *   - alloc_pid_kernel() 允许 1..99，仅由内核引导路径（boot_services）
 *     调用。
 *
 * 用户态进程无论权限等级如何，都不可能通过普通系统调用触达此接口，
 * 因此不会破坏"PID 1..99 属于系统保留区"这一硬性约束。
 *
 * 返回值语义同 alloc_pid()。
 */
int64_t alloc_pid_kernel(uint64_t requested);

#endif /* OMNIBRIDGE_PID_H */