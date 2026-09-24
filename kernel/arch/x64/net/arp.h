/*===OmniBridgeOs/kernel/arch/x64/net/arp.h===*/
#ifndef OMNIBRIDGE_NET_ARP_H
#define OMNIBRIDGE_NET_ARP_H

#include <stdint.h>
#include "net.h"

#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP  0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

#define ARP_CACHE_MAX 32
#define ARP_TIMEOUT_TICKS 6000   /* 60s @ 100Hz */

struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t op;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  valid;
    uint64_t last_tick;
};

void arp_init(void);
void arp_rx(struct netif *nif, struct netbuf *nb);

/* 查找/学习缓存 */
int  arp_lookup(struct netif *nif, uint32_t ip, uint8_t out_mac[6]);
void arp_learn(struct netif *nif, uint32_t ip, const uint8_t mac[6]);

/* 主动发起请求（用于解析目标 MAC），返回 0 表示已在缓存 */
int  arp_resolve(struct netif *nif, uint32_t ip);

void arp_dump(void);

#endif /* OMNIBRIDGE_NET_ARP_H */
/*===OmniBridgeOs/kernel/arch/x64/net/arp.h 结束===*/