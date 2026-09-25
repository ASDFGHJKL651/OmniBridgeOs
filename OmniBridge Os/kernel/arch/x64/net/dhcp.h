/*===OmniBridgeOs/kernel/arch/x64/net/dhcp.h===*/
#ifndef OMNIBRIDGE_NET_DHCP_H
#define OMNIBRIDGE_NET_DHCP_H

#include <stdint.h>
#include "net.h"

void dhcp_init(void);
int  dhcp_start(struct netif *nif);
int  dhcp_is_done(void);

/* 手动构造并处理一个 DHCP 响应（用于自检） */
int  dhcp_handle_offer_ack(struct netif *nif, const void *payload,
                           uint32_t len, uint8_t is_ack);

#endif /* OMNIBRIDGE_NET_DHCP_H */
/*===OmniBridgeOs/kernel/arch/x64/net/dhcp.h 结束===*/