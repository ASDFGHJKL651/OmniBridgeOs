/*===OmniBridgeOs/kernel/arch/x64/net/ipv4.c===*/
#include "ipv4.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "route.h"
#include "firewall.h"
#include "csprng.h"
#include "serial.h"

void ipv4_init(void)
{
    serial_printf("[IPv4] init: proto=ICMP/UDP/TCP\n");
}

/*
 * 16 位 Internet 反码校验和（RFC 1071）。
 *
 *   data —— 指向连续的、以**网络字节序**（大端）排列的字节流。
 *          函数按 (p[0] << 8) | p[1] 逐个读入 16 位字。
 *
 *   返回值 —— **host order** 的 16 位数值。
 *            调用方若写入网络包，必须用 net_htons() 转换后再写。
 */
uint16_t ip_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

/*
 * 带伪首部的 16 位 Internet 反码校验和（RFC 768 §3、RFC 793 §3.1）。
 *
 *   src / dst —— ★ **必须传 host order** 的 32 位 IP 地址。
 *
 *       语义说明（人工必须审查）：
 *         伪首部中源/目的 IP 按网络字节序（大端）排列 —— IP 的第一个
 *         字节是第一个 16 位字的最高字节。host order 的 uint32_t 数值
 *         高位恰对应 IP 的第一个字节，所以：
 *           (src >> 16) & 0xFFFF  ==  网络包中前 2 字节组成的 16 位字
 *           src & 0xFFFF          ==  网络包中后 2 字节组成的 16 位字
 *
 *       ★ 历史 bug（第 18A 步 v10→v11 修复）：
 *         曾经所有调用方都传 net_htonl(host_ip)，导致两处 16 位字完全
 *         错位。与 loopback/自检路径自洽（因为两端都用同一错误算法），
 *         但与 QEMU SLIRP 通信时必定失败 —— DHCP OFFER 的 UDP 校验和
 *         永远对不上。修复后调用方直接传 host order。
 *
 *   返回值 —— **host order** 的 16 位数值；写入网络包时用 net_htons()。
 */
uint16_t ip_checksum_pseudo(uint32_t src, uint32_t dst,
                            uint8_t proto, uint32_t l4_len,
                            const void *l4_hdr, uint32_t l4_hdr_len,
                            const void *l4_payload, uint32_t l4_payload_len)
{
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += proto;
    sum += l4_len;

    const uint8_t *p = (const uint8_t *)l4_hdr;
    uint32_t n = l4_hdr_len;
    while (n > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (uint32_t)p[0] << 8;

    p = (const uint8_t *)l4_payload;
    n = l4_payload_len;
    while (n > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (uint32_t)p[0] << 8;

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

/*
 * ★ 本轮修复：广播地址检测。
 *
 *   DHCP DISCOVER / REQUEST 使用受限广播 255.255.255.255 或子网广播。
 *   这类目标必须在链路层使用广播 MAC（FF:FF:FF:FF:FF:FF），
 *   而不是走 ARP 解析（ARP 无法解析广播地址）。
 */
static int is_broadcast_addr(uint32_t dst, uint32_t my_ip, uint32_t my_mask)
{
    if (dst == 0xFFFFFFFFu) return 1;      /* 受限广播 */
    if (my_ip && my_mask) {
        uint32_t bcast = (my_ip & my_mask) | (~my_mask);
        if (dst == bcast) return 1;        /* 子网广播 */
    }
    return 0;
}

void ipv4_rx(struct netif *nif, struct netbuf *nb)
{
    if (nb->len < sizeof(struct ipv4_hdr)) { netbuf_free(nb); return; }
    struct ipv4_hdr *h = (struct ipv4_hdr *)nb->data;

    uint8_t ver = (uint8_t)(h->ihl_version >> 4);
    uint8_t ihl = (uint8_t)(h->ihl_version & 0x0F);
    if (ver != 4 || ihl < 5) { netbuf_free(nb); return; }
    uint32_t hlen = (uint32_t)ihl * 4;
    if (nb->len < hlen) { netbuf_free(nb); return; }

    uint16_t tot = net_ntohs(h->total_len);
    if (tot < hlen || tot > nb->len) { netbuf_free(nb); return; }

    uint16_t cks = ip_checksum(nb->data, hlen);
    if (cks != 0) {
        serial_printf("[IPv4] bad header checksum (calc=0x%04x)\n",
                      (unsigned)cks);
        netbuf_free(nb); return;
    }

    uint16_t frag = net_ntohs(h->frag_off);
    if ((frag & 0x3FFFu) != 0 || (frag & 0x2000u)) {
        serial_printf("[IPv4] WARN: fragment dropped\n");
        netbuf_free(nb); return;
    }

    uint32_t src = net_ntohl(h->src);
    uint32_t dst = net_ntohl(h->dst);
    uint8_t  proto = h->proto;

    if (fw_hook(nif->ns, FW_DIR_IN, proto, src, dst, 0, 0, 0) != 0) {
        netbuf_free(nb); return;
    }

    netbuf_pull(nb, hlen);

    switch (proto) {
    case IP_PROTO_ICMP: icmp_rx(nif, src, nb); return;
    case IP_PROTO_UDP:  udp_rx(nif, src, dst, nb); return;
    case IP_PROTO_TCP:  tcp_rx(nif, src, dst, nb); return;
    default:
        netbuf_free(nb);
        return;
    }
}

int ipv4_send(struct netif *nif, uint32_t dst_ip, uint8_t proto,
              struct netbuf *payload)
{
    if (!nif || !payload) return -1;

    struct netns *ns = nif->ns;
    uint32_t nh = dst_ip;
    struct netif *onif = nif;
    struct route_entry *rt = 0;
    if (ns && route_lookup(ns, dst_ip, &rt) == 0 && rt) {
        if (rt->nif) onif = rt->nif;
        if (rt->flags & RT_FLAG_GATEWAY) nh = rt->gateway;
    }

    if (fw_hook(onif->ns, FW_DIR_OUT, proto, onif->ip, dst_ip, 0, 0, 0) != 0) {
        netbuf_free(payload);
        return -1;
    }

    uint32_t plen = payload->len;
    if (netbuf_push(payload, sizeof(struct ipv4_hdr)) != 0) {
        netbuf_free(payload); return -1;
    }
    struct ipv4_hdr *h = (struct ipv4_hdr *)payload->data;
    h->ihl_version = 0x45;
    h->tos         = 0;
    h->total_len   = net_htons((uint16_t)(sizeof(struct ipv4_hdr) + plen));
    h->id          = net_htons(csprng_u16());
    h->frag_off    = 0;
    h->ttl         = 64;
    h->proto       = proto;
    h->checksum    = 0;
    h->src         = net_htonl(onif->ip);
    h->dst         = net_htonl(dst_ip);

    uint16_t c = ip_checksum(h, sizeof(struct ipv4_hdr));
    h->checksum = net_htons(c);

    uint8_t mac[6];
    if (is_broadcast_addr(dst_ip, onif->ip, onif->mask)) {
        mac[0] = mac[1] = mac[2] = mac[3] = mac[4] = mac[5] = 0xFF;
    } else if (arp_lookup(onif, nh, mac) != 0) {
        arp_resolve(onif, nh);
        netbuf_free(payload);
        return -1;
    }

    return ethernet_send(onif, mac, ETH_TYPE_IPV4, payload);
}
/*===OmniBridgeOs/kernel/arch/x64/net/ipv4.c 结束===*/