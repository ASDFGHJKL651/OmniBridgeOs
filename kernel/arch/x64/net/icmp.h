/*===OmniBridgeOs/kernel/arch/x64/net/icmp.h===*/
#ifndef OMNIBRIDGE_NET_ICMP_H
#define OMNIBRIDGE_NET_ICMP_H

#include <stdint.h>
#include "net.h"

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

void icmp_init(void);
void icmp_rx(struct netif *nif, uint32_t src_ip, struct netbuf *nb);

/* 发送 Echo Request；返回 0 成功 */
int  icmp_send_echo(struct netif *nif, uint32_t dst_ip,
                    uint16_t id, uint16_t seq,
                    const void *payload, uint32_t payload_len);

/* 简易 ping：同步等待，最多 timeout_ticks；成功返回 0 */
int  icmp_ping(struct netif *nif, uint32_t dst_ip,
               uint32_t payload_len, uint32_t timeout_ticks);

#endif /* OMNIBRIDGE_NET_ICMP_H */
/*===OmniBridgeOs/kernel/arch/x64/net/icmp.h 结束===*/