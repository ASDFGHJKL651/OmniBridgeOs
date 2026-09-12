#ifndef OMNIBRIDGE_SLAB_H
#define OMNIBRIDGE_SLAB_H

/*
 * 内核 SLAB 分配器。
 *
 * 设计目标（人工必须审查）：
 *   - 提供 kmem_cache_create/destroy/alloc/free 四个标准接口。
 *   - 每个 cache 拥有一个 partial slab 链表；分配优先从 partial 取，空了
 *     则从伙伴系统申请一个新 slab。
 *   - 每个 slab 的空闲对象通过链表链接，链表头存放在对应 struct page 的
 *     slab_free 字段中；slab 归属的 cache 存放在 slab_cache 字段。
 *   - 全部操作在 cache 自己的自旋锁保护下进行；跨 cache 的操作独立。
 *   - 本步不做每 CPU 缓存；正确性优先，后续步骤可在此结构上附加每 CPU
 *     缓存（占位字段已通过 percpu.h 抽象，不影响本文件）。
 *
 * 与伙伴系统的关系：
 *   - slab 的物理存储由 pmm_alloc_pages(slab_order) 提供。
 *   - slab 释放后通过 pmm_free_pages() 归还。
 *
 * 大小与对齐：
 *   - 对象大小向上对齐到 align（若 align==0 则默认为 8）。
 *   - 每个 slab 至少能装下 2 个对象，否则放大 slab_order。
 */

#include <stddef.h>
#include <stdint.h>
#include "spinlock.h"

#define SLAB_MAX_NAME 32
#define SLAB_MAX_CACHES 64

/* flags 预留：目前不使用，全部忽略 */
#define SLAB_HWCACHE_ALIGN 0x0001
#define SLAB_ZERO          0x0002

struct kmem_cache {
    char     name[SLAB_MAX_NAME];
    size_t   object_size;        /* 对齐后的对象字节数 */
    size_t   objects_per_slab;   /* 一个 slab 能装多少对象 */
    size_t   slab_order;         /* 伙伴系统 order（slab 字节数 = PAGE_SIZE<<order） */
    struct page *partial;        /* 有空闲对象的 slab 链表 */
    spinlock_t   lock;
    uint32_t     magic;          /* 用于 destroy 校验与调试 */
};

#define SLAB_MAGIC 0x534C4142u   /* "SLAB" */

/* 初始化 SLAB 子系统（幂等，可多次调用） */
void slab_init(void);

/* 创建一个 cache；失败返回 NULL。name 会被截断到 SLAB_MAX_NAME-1 */
struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                     size_t align, uint32_t flags);

/* 销毁一个 cache：要求其全部对象已释放（仅检查 partial 为空） */
void kmem_cache_destroy(struct kmem_cache *c);

void *kmem_cache_alloc(struct kmem_cache *c);
void  kmem_cache_free (struct kmem_cache *c, void *obj);

#endif /* OMNIBRIDGE_SLAB_H */