/*===OmniBridgeOs/kernel/arch/x64/shm.h===*/
#ifndef OMNIBRIDGE_SHM_H
#define OMNIBRIDGE_SHM_H

#include <stdint.h>
#include "spinlock.h"

struct task_t;

struct shm_region {
    void       *addr;
    uint64_t    size;
    int         order;
    uint32_t    refcount;
    uint32_t    shm_id;
    spinlock_t  lock;
};

/* 每个 task 的共享内存映射记录 */
struct shm_mapping {
    struct shm_region *region;
    uint64_t           uaddr;
    uint64_t           size;
    uint64_t           phys_base;   /* 第一页物理地址 */
    struct shm_mapping *next;
};

struct shm_region *shm_create(uint64_t size);
void shm_retain(struct shm_region *r);
void shm_release(struct shm_region *r);
void *shm_addr(struct shm_region *r);
uint32_t shm_refcount(struct shm_region *r);

/* ★ 第 18D 步：映射到 task 的用户空间 */
int shm_map_into_task(struct task_t *t, struct shm_region *r, uint64_t uaddr);

/* ★ 第 18D 步：系统调用接口 */
int64_t shm_sys_create(struct task_t *cur, uint64_t size);
int64_t shm_sys_map(struct task_t *cur, uint64_t shm_id, uint64_t uaddr);
int     shm_sys_unmap(struct task_t *cur, uint64_t uaddr, uint64_t size);

/* ★ 第 18D 步：task 退出时释放所有映射 */
void shm_release_mappings(struct task_t *t);

/* ★ 全局 shm 表（用于 shm_sys_map 通过 shm_id 查找） */
struct shm_region *shm_find_by_id(uint32_t shm_id);

#endif /* OMNIBRIDGE_SHM_H */