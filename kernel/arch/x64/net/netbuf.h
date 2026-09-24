/*===OmniBridgeOs/kernel/arch/x64/net/netbuf.h===*/
/*
 * 网络缓冲区 netbuf（第 18A 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 引用计数使用 spinlock 保护，允许中断与进程上下文并发访问。
 *   - alloc/free 使用固定池 + kmalloc 回退；池上限 NETBUF_POOL_MAX。
 *   - push/pull 修改 data 指针；headroom/tailroom 由池布局决定。
 */
#ifndef OMNIBRIDGE_NET_NETBUF_H
#define OMNIBRIDGE_NET_NETBUF_H

#include <stdint.h>
#include "spinlock.h"

struct netns;

#define NETBUF_DATA_SIZE  2048
#define NETBUF_HEADROOM   128
#define NETBUF_TAILROOM   128

struct netbuf {
    uint8_t  *data;              /* 当前首字节指针 */
    uint32_t  len;               /* 有效字节数 */
    uint32_t  cap;               /* 从 data 起可用容量 */
    uint32_t  refcount;
    uint32_t  _pad;
    struct netns *ns;            /* 所属命名空间，可为 NULL */
    uint8_t  *base;              /* 分配起点（用于 free 时还原） */
    uint32_t  base_cap;          /* 总容量 */
    spinlock_t lock;
};

void  netbuf_init(void);

struct netbuf *netbuf_alloc(struct netns *ns, uint32_t size);
struct netbuf *netbuf_clone(const struct netbuf *src);
void  netbuf_retain(struct netbuf *nb);
void  netbuf_free(struct netbuf *nb);

/* 在头部预留空间：成功返回 0，失败 -1 */
int   netbuf_push(struct netbuf *nb, uint32_t bytes);
/* 从尾部追加空间（调整 len）：成功返回 0 */
int   netbuf_put(struct netbuf *nb, uint32_t bytes);
/* 从头部移除 bytes（data += bytes, len -= bytes） */
void  netbuf_pull(struct netbuf *nb, uint32_t bytes);

uint32_t netbuf_headroom(const struct netbuf *nb);
uint32_t netbuf_tailroom(const struct netbuf *nb);

#endif /* OMNIBRIDGE_NET_NETBUF_H */
/*===OmniBridgeOs/kernel/arch/x64/net/netbuf.h 结束===*/