/*===OmniBridgeOs/kernel/arch/x64/net/ipv4.h===*/
#ifndef OMNIBRIDGE_NET_IPV4_H
#define OMNIBRIDGE_NET_IPV4_H

#include <stdint.h>
#include "net.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct ipv4_hdr {
    uint8_t  ihl_version;    /* 高 4 位版本，低 4 位 IHL */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

void ipv4_init(void);
void ipv4_rx(struct netif *nif, struct netbuf *nb);

/* 发送 IP 包：dst_ip 为主机字节序，proto 已设置；payload 已在 nb 中 */
int  ipv4_send(struct netif *nif, uint32_t dst_ip, uint8_t proto,
               struct netbuf *payload);

/* 16 位反码校验和（用于 IP 头、ICMP、UDP、TCP） */
uint16_t ip_checksum(const void *data, uint32_t len);
uint16_t ip_checksum_pseudo(uint32_t src, uint32_t dst,
                            uint8_t proto, uint32_t l4_len,
                            const void *l4_hdr, uint32_t l4_hdr_len,
                            const void *l4_payload, uint32_t l4_payload_len);

#endif /* OMNIBRIDGE_NET_IPV4_H */
/*===OmniBridgeOs/kernel/arch/x64/net/ipv4.h 结束===*/