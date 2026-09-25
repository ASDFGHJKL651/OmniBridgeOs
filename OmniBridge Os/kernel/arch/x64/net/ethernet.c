/*===OmniBridgeOs/kernel/arch/x64/net/ethernet.c===*/
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "serial.h"

void ethernet_init(void)
{
    serial_printf("[ETH] init: ethertypes IPv4/ARP/IPv6\n");
}

void ethernet_rx(struct netif *nif, struct netbuf *nb)
{
    if (!nif || !nb) return;
    if (nb->len < ETH_HLEN) { netbuf_free(nb); return; }

    const struct eth_hdr *h = (const struct eth_hdr *)nb->data;
    uint16_t et = net_ntohs(h->ethertype);

    /* 移除以太网首部 */
    netbuf_pull(nb, ETH_HLEN);

    switch (et) {
    case ETH_TYPE_IPV4: ipv4_rx(nif, nb); return;
    case ETH_TYPE_ARP:  arp_rx(nif, nb);  return;
    case ETH_TYPE_IPV6: ipv6_rx(nif, nb); return;
    default:
        netbuf_free(nb);
        return;
    }
}

int ethernet_send(struct netif *nif, const uint8_t dst_mac[6],
                  uint16_t ethertype, struct netbuf *nb)
{
    if (!nif || !nb) return -1;

    if (netbuf_push(nb, ETH_HLEN) != 0) {
        netbuf_free(nb);
        return -1;
    }
    struct eth_hdr *h = (struct eth_hdr *)nb->data;
    for (int i = 0; i < 6; ++i) h->dst[i] = dst_mac[i];
    for (int i = 0; i < 6; ++i) h->src[i] = nif->mac[i];
    h->ethertype = net_htons(ethertype);

    if (!nif->xmit) { netbuf_free(nb); return -1; }
    return nif->xmit(nif, nb);
}
/*===OmniBridgeOs/kernel/arch/x64/net/ethernet.c 结束===*/