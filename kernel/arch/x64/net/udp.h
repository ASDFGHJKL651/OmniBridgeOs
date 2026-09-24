/*===OmniBridgeOs/kernel/arch/x64/net/udp.h===*/
#ifndef OMNIBRIDGE_NET_UDP_H
#define OMNIBRIDGE_NET_UDP_H

#include <stdint.h>
#include "net.h"
#include "netns.h"

#define UDP_HDR_LEN 8
#define UDP_MAX_BINDINGS NETNS_MAX_UDP_BINDINGS

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

typedef void (*udp_recv_cb)(struct netif *nif, uint32_t src_ip,
                            uint16_t src_port, uint16_t dst_port,
                            const void *data, uint32_t len, void *arg);

struct udp_binding {
    uint16_t local_port;
    uint32_t local_ip;     /* 0 = any */
    udp_recv_cb cb;
    void    *arg;
    struct netns *ns;      /* ★ 所属命名空间 */
    uint8_t  used;
};

void udp_init(void);

int  udp_bind(struct netns *ns, uint16_t local_port, uint32_t local_ip,
              udp_recv_cb cb, void *arg);
void udp_unbind(struct netns *ns, uint16_t local_port);

void udp_rx(struct netif *nif, uint32_t src_ip, uint32_t dst_ip,
            struct netbuf *nb);

int  udp_send(struct netif *nif, uint32_t dst_ip,
              uint16_t src_port, uint16_t dst_port,
              const void *payload, uint32_t len);

/* 分配一个临时本地端口（用于 DHCP 客户端、DNS 查询） */
uint16_t udp_alloc_ephemeral(void);

#endif /* OMNIBRIDGE_NET_UDP_H */
/*===OmniBridgeOs/kernel/arch/x64/net/udp.h 结束===*/