/*===OmniBridgeOs/kernel/arch/x64/pipefs.h===*/
#ifndef OMNIBRIDGE_PIPEFS_H
#define OMNIBRIDGE_PIPEFS_H

#include <stdint.h>
#include "spinlock.h"
#include "vfs.h"

struct task_t;
struct thread;

#define PIPE_BUF_SIZE  4096
#define PIPE_MAX_WAIT  16

struct pipe_wait_node {
    struct thread *th;
    struct pipe_wait_node *next;
};

/*
 * ★ 第 18D 步修复：struct pipe_inode 拆分为"控制块 + 独立 data"。
 *
 * 原因：原定义内嵌 uint8_t data[4096]，加上其它字段后 sizeof ≈ 4.2KB，
 * 超过 kmalloc 单次上限（PAGE_SIZE = 4096），kzalloc 直接返回 NULL，
 * 导致 pipefs_create 全部失败。
 *
 * 修复：控制块只保留指针字段（≈ 72 字节），data 用 kmalloc(4096) 单独分配。
 */
struct pipe_inode {
    uint8_t  *data;              /* kmalloc(PIPE_BUF_SIZE) */
    uint32_t  head;
    uint32_t  tail;
    uint32_t  len;

    uint32_t  readers;
    uint32_t  writers;

    spinlock_t lock;

    struct pipe_wait_node *read_waiters;
    struct pipe_wait_node *write_waiters;
    uint32_t  read_wait_count;
    uint32_t  write_wait_count;
};

int  pipefs_init(void);
int  pipefs_create(struct task_t *cur, uint64_t uaddr);
int64_t pipefs_read(struct vfs_file *f, void *buf, uint64_t count);
int64_t pipefs_write(struct vfs_file *f, const void *buf, uint64_t count);
int     pipefs_close(struct vfs_file *f);
int     pipefs_poll(struct vfs_file *f, short events);

#endif /* OMNIBRIDGE_PIPEFS_H */
/*===OmniBridgeOs/kernel/arch/x64/pipefs.h 结束===*/