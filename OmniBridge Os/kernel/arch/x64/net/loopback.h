/*===OmniBridgeOs/kernel/arch/x64/net/loopback.h===*/
#ifndef OMNIBRIDGE_NET_LOOPBACK_H
#define OMNIBRIDGE_NET_LOOPBACK_H

#include "net.h"

#define LOOPBACK_MTU 65536

void loopback_init(struct netns *ns);
struct netif *loopback_iface(struct netns *ns);

#endif /* OMNIBRIDGE_NET_LOOPBACK_H */
/*===OmniBridgeOs/kernel/arch/x64/net/loopback.h 结束===*/