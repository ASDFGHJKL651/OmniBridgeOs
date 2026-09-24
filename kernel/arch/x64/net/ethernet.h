/*===OmniBridgeOs/kernel/arch/x64/net/ethernet.h===*/
#ifndef OMNIBRIDGE_NET_ETHERNET_H
#define OMNIBRIDGE_NET_ETHERNET_H

#include <stdint.h>
#include "net.h"

#define ETH_HLEN        14
#define ETH_TYPE_IPV4   0x0800u
#define ETH_TYPE_ARP    0x0806u
#define ETH_TYPE_IPV6   0x86DDu
#define ETH_BROADCAST   "\xff\xff\xff\xff\xff\xff"

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;
} __attribute__((packed));

void ethernet_init(void);

/* 接收入口（由驱动调用，nb 的 data 指向以太网首部） */
void ethernet_rx(struct netif *nif, struct netbuf *nb);

/* 发送：填 src/dst/type；调用 nif->xmit */
int  ethernet_send(struct netif *nif, const uint8_t dst_mac[6],
                   uint16_t ethertype, struct netbuf *nb);

#endif /* OMNIBRIDGE_NET_ETHERNET_H */
/*===OmniBridgeOs/kernel/arch/x64/net/ethernet.h 结束===*/