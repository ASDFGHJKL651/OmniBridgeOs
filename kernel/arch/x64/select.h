/*===OmniBridgeOs/kernel/arch/x64/select.h===*/
#ifndef OMNIBRIDGE_SELECT_H
#define OMNIBRIDGE_SELECT_H

#include <stdint.h>
#include "task.h"

#define POLLIN   0x001
#define POLLOUT  0x002
#define POLLERR  0x004
#define POLLHUP  0x010

struct pollfd {
    int   fd;
    short events;
    short revents;
};

void select_init(void);

/* poll 系统调用主体：读用户态 pollfd 数组，返回就绪 fd 数。 */
int64_t select_poll(struct task_t *cur, uint64_t fds_uaddr,
                    uint32_t nfds, int32_t timeout_ms);

/* select 系统调用最小实现。 */
int64_t select_select(struct task_t *cur, uint32_t nfds,
                      uint64_t rfd_uaddr, uint64_t wfd_uaddr);

/* ★ 状态变化时唤醒全局 poll 等待者（由 pipe/socket 调用）。
 * 本实现为简化：不做实际唤醒，select_poll 内部轮询。 */
void select_notify_wakeup(void);

#endif /* OMNIBRIDGE_SELECT_H */