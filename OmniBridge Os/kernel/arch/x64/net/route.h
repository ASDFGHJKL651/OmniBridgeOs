/*===OmniBridgeOs/kernel/arch/x64/net/route.h===*/
#ifndef OMNIBRIDGE_NET_ROUTE_H
#define OMNIBRIDGE_NET_ROUTE_H

#include <stdint.h>
#include "net.h"

#define RT_FLAG_HOST    0x01u
#define RT_FLAG_GATEWAY 0x02u
#define RT_FLAG_DEFAULT 0x04u

struct route_entry {
    uint32_t dst;        /* 主机字节序 */
    uint32_t mask;       /* 主机字节序 */
    uint32_t gateway;    /* 主机字节序，0 表示直连 */
    struct netif *nif;
    uint8_t  flags;
    uint8_t  _pad[3];
    struct route_entry *next;
};

void route_init(void);
int  route_add(struct netns *ns, uint32_t dst, uint32_t mask,
               uint32_t gw, struct netif *nif, uint8_t flags);
void route_clear(struct netns *ns);

/* 最长前缀匹配：返回 0 命中，-1 无匹配 */
int  route_lookup(struct netns *ns, uint32_t dst,
                  struct route_entry **out);

void route_dump(struct netns *ns);

#endif /* OMNIBRIDGE_NET_ROUTE_H */
/*===OmniBridgeOs/kernel/arch/x64/net/route.h 结束===*/