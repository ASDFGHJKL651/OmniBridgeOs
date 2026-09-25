/*===OmniBridgeOs/kernel/arch/x64/net/dns.h===*/
#ifndef OMNIBRIDGE_NET_DNS_H
#define OMNIBRIDGE_NET_DNS_H

#include <stdint.h>
#include "net.h"

#define DNS_CACHE_MAX 16

struct dns_cache_entry {
    char     name[64];
    uint32_t ip;
    uint64_t expire_tick;
    uint8_t  valid;
};

void dns_init(void);
int  dns_resolve(struct netif *nif, const char *name, uint32_t *out_ip);

/* ★ AAAA 查询：成功返回 0，out_ip6 填 16 字节 IPv6 地址 */
int  dns_resolve_aaaa(struct netif *nif, const char *name, uint8_t out_ip6[16]);

/* 自检：模拟 DNS 响应 */
int  dns_handle_response(const uint8_t *pkt, uint32_t len, uint32_t *out_ip);
int  dns_handle_response_aaaa(const uint8_t *pkt, uint32_t len,
                              uint8_t out_ip6[16]);

void dns_dump_cache(void);

#endif /* OMNIBRIDGE_NET_DNS_H */
/*===OmniBridgeOs/kernel/arch/x64/net/dns.h 结束===*/