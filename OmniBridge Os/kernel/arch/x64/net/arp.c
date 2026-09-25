/*===OmniBridgeOs/kernel/arch/x64/net/arp.c===*/
#include "arp.h"
#include "ethernet.h"
#include "serial.h"
#include "spinlock.h"

static struct arp_entry g_cache[ARP_CACHE_MAX];
static spinlock_t       g_lock = SPINLOCK_INIT;
static uint64_t         g_ticks = 0;

void arp_init(void)
{
    spin_lock_init(&g_lock);
    for (int i = 0; i < ARP_CACHE_MAX; ++i) {
        g_cache[i].valid = 0;
        g_cache[i].ip = 0;
        g_cache[i].last_tick = 0;
        for (int k = 0; k < 6; ++k) g_cache[i].mac[k] = 0;
    }
    serial_printf("[ARP] init: cache=%d timeout=%u ticks\n",
                  ARP_CACHE_MAX, (unsigned)ARP_TIMEOUT_TICKS);
}

void arp_learn(struct netif *nif, uint32_t ip, const uint8_t mac[6])
{
    (void)nif;
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    g_ticks++;

    int slot = -1;
    for (int i = 0; i < ARP_CACHE_MAX; ++i) {
        if (g_cache[i].valid && g_cache[i].ip == ip) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < ARP_CACHE_MAX; ++i) {
            if (!g_cache[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) {
        /* 直接覆盖 0 号 */
        slot = 0;
    }
    g_cache[slot].ip = ip;
    for (int k = 0; k < 6; ++k) g_cache[slot].mac[k] = mac[k];
    g_cache[slot].valid = 1;
    g_cache[slot].last_tick = g_ticks;
    spin_unlock_irqrestore(&g_lock, fl);
}

int arp_lookup(struct netif *nif, uint32_t ip, uint8_t out_mac[6])
{
    (void)nif;
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    int found = 0;
    for (int i = 0; i < ARP_CACHE_MAX; ++i) {
        if (g_cache[i].valid && g_cache[i].ip == ip) {
            if ((g_ticks - g_cache[i].last_tick) > ARP_TIMEOUT_TICKS) {
                g_cache[i].valid = 0;
                break;
            }
            for (int k = 0; k < 6; ++k) out_mac[k] = g_cache[i].mac[k];
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&g_lock, fl);
    return found ? 0 : -1;
}

static int arp_send_req(struct netif *nif, uint32_t tpa)
{
    struct netbuf *nb = netbuf_alloc(nif->ns, sizeof(struct arp_hdr));
    if (!nb) return -1;
    struct arp_hdr *a = (struct arp_hdr *)nb->data;
    a->htype = net_htons(ARP_HTYPE_ETH);
    a->ptype = net_htons(ARP_PTYPE_IP);
    a->hlen  = 6;
    a->plen  = 4;
    a->op    = net_htons(ARP_OP_REQUEST);
    for (int i = 0; i < 6; ++i) a->sha[i] = nif->mac[i];
    a->spa = net_htonl(nif->ip);
    for (int i = 0; i < 6; ++i) a->tha[i] = 0;
    a->tpa = net_htonl(tpa);
    nb->len = sizeof(struct arp_hdr);

    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    serial_printf("[ARP] request who-has 0x%08x tell 0x%08x\n",
                  (unsigned)tpa, (unsigned)nif->ip);
    return ethernet_send(nif, bcast, ETH_TYPE_ARP, nb);
}

int arp_resolve(struct netif *nif, uint32_t ip)
{
    uint8_t mac[6];
    if (arp_lookup(nif, ip, mac) == 0) return 0;
    arp_send_req(nif, ip);
    return -1;
}

void arp_rx(struct netif *nif, struct netbuf *nb)
{
    if (nb->len < sizeof(struct arp_hdr)) { netbuf_free(nb); return; }
    struct arp_hdr *a = (struct arp_hdr *)nb->data;
    uint16_t op = net_ntohs(a->op);
    uint32_t spa = net_ntohl(a->spa);
    uint32_t tpa = net_ntohl(a->tpa);

    arp_learn(nif, spa, a->sha);

    if (op == ARP_OP_REQUEST) {
        /* 若目标为本机 IP，回复 */
        if (tpa == nif->ip) {
            struct netbuf *r = netbuf_alloc(nif->ns, sizeof(struct arp_hdr));
            if (r) {
                struct arp_hdr *ra = (struct arp_hdr *)r->data;
                ra->htype = net_htons(ARP_HTYPE_ETH);
                ra->ptype = net_htons(ARP_PTYPE_IP);
                ra->hlen  = 6; ra->plen = 4;
                ra->op    = net_htons(ARP_OP_REPLY);
                for (int i = 0; i < 6; ++i) ra->sha[i] = nif->mac[i];
                ra->spa = net_htonl(nif->ip);
                for (int i = 0; i < 6; ++i) ra->tha[i] = a->sha[i];
                ra->tpa = net_htonl(spa);
                r->len = sizeof(struct arp_hdr);
                ethernet_send(nif, a->sha, ETH_TYPE_ARP, r);
            }
        }
    }
    netbuf_free(nb);
}

void arp_dump(void)
{
    serial_printf("[ARP] cache dump:\n");
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    for (int i = 0; i < ARP_CACHE_MAX; ++i) {
        if (!g_cache[i].valid) continue;
        serial_printf("  0x%08x -> %02x:%02x:%02x:%02x:%02x:%02x (age=%llu)\n",
                      (unsigned)g_cache[i].ip,
                      g_cache[i].mac[0], g_cache[i].mac[1], g_cache[i].mac[2],
                      g_cache[i].mac[3], g_cache[i].mac[4], g_cache[i].mac[5],
                      (unsigned long long)(g_ticks - g_cache[i].last_tick));
    }
    spin_unlock_irqrestore(&g_lock, fl);
}
/*===OmniBridgeOs/kernel/arch/x64/net/arp.c 结束===*/