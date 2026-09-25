/*===OmniBridgeOs/kernel/arch/x64/net/udp.c===*/
#include "udp.h"
#include "ipv4.h"
#include "firewall.h"
#include "netns.h"
#include "kmalloc.h"
#include "serial.h"
#include "spinlock.h"

static spinlock_t g_eph_lock = SPINLOCK_INIT;
static uint16_t   g_eph_cursor = 40000;

void udp_init(void)
{
    spin_lock_init(&g_eph_lock);
    serial_printf("[UDP] init: per-ns bindings, max=%d\n", UDP_MAX_BINDINGS);
}

uint16_t udp_alloc_ephemeral(void)
{
    uint64_t fl;
    spin_lock_irqsave(&g_eph_lock, &fl);
    uint16_t p = g_eph_cursor++;
    if (g_eph_cursor > 60000) g_eph_cursor = 40000;
    spin_unlock_irqrestore(&g_eph_lock, fl);
    return p;
}

int udp_bind(struct netns *ns, uint16_t local_port, uint32_t local_ip,
             udp_recv_cb cb, void *arg)
{
    if (!ns) ns = netns_default();
    if (!ns) return -1;

    uint64_t fl;
    spin_lock_irqsave(&ns->udp_lock, &fl);

    int slot = -1;
    for (int i = 0; i < UDP_MAX_BINDINGS; ++i) {
        struct udp_binding *b = ns->udp_binds[i];
        if (b && b->used && b->local_port == local_port) { slot = i; break; }
    }

    if (slot < 0) {
        for (int i = 0; i < UDP_MAX_BINDINGS; ++i) {
            if (!ns->udp_binds[i]) {
                ns->udp_binds[i] = (struct udp_binding *)
                    kzalloc(sizeof(struct udp_binding));
                if (!ns->udp_binds[i]) {
                    spin_unlock_irqrestore(&ns->udp_lock, fl);
                    return -1;
                }
                slot = i;
                break;
            }
            if (!ns->udp_binds[i]->used) { slot = i; break; }
        }
    }

    if (slot < 0) {
        spin_unlock_irqrestore(&ns->udp_lock, fl);
        return -1;
    }

    struct udp_binding *b = ns->udp_binds[slot];
    b->used       = 1;
    b->local_port = local_port;
    b->local_ip   = local_ip;
    b->cb         = cb;
    b->arg        = arg;
    b->ns         = ns;

    ns->udp_count++;
    spin_unlock_irqrestore(&ns->udp_lock, fl);
    return 0;
}

void udp_unbind(struct netns *ns, uint16_t local_port)
{
    if (!ns) ns = netns_default();
    if (!ns) return;

    uint64_t fl;
    spin_lock_irqsave(&ns->udp_lock, &fl);
    for (int i = 0; i < UDP_MAX_BINDINGS; ++i) {
        struct udp_binding *b = ns->udp_binds[i];
        if (b && b->used && b->local_port == local_port) {
            b->used = 0; b->cb = 0; b->arg = 0;
            if (ns->udp_count > 0) ns->udp_count--;
            break;
        }
    }
    spin_unlock_irqrestore(&ns->udp_lock, fl);
}

/*
 * ★ 修复（v11）：去掉 net_htonl。
 *
 *   ip_checksum_pseudo 的 src/dst 参数期望 **host order** 的 32 位 IP。
 *   原因：伪首部中的 IP 按网络字节序（大端）排列，而 host order 的
 *   uint32_t 的高位字节恰好是 IP 的第一个字节。之前传 net_htonl(ip)
 *   会导致两处 16 位字完全错位，所有校验和都是错的（与 SLIRP 不兼容）。
 */
static uint16_t udp_compute_checksum(uint32_t src_ip, uint32_t dst_ip,
                                     const void *udp_pkt, uint32_t len)
{
    return ip_checksum_pseudo(
        src_ip, dst_ip,
        IP_PROTO_UDP, len,
        0, 0,
        udp_pkt, len);
}

void udp_rx(struct netif *nif, uint32_t src_ip, uint32_t dst_ip,
            struct netbuf *nb)
{
    if (nb->len < UDP_HDR_LEN) { netbuf_free(nb); return; }

    struct udp_hdr *h = (struct udp_hdr *)nb->data;
    uint16_t sp   = net_ntohs(h->src_port);
    uint16_t dp   = net_ntohs(h->dst_port);
    uint16_t ulen = net_ntohs(h->length);
    uint16_t cks  = net_ntohs(h->checksum);

    if (ulen < UDP_HDR_LEN || ulen > nb->len) { netbuf_free(nb); return; }

    if (cks != 0) {
        uint16_t calc = udp_compute_checksum(src_ip, dst_ip, nb->data, ulen);
        if (calc != 0) {
            serial_printf("[UDP] bad checksum src=0x%08x dst=0x%08x dp=%u\n",
                          (unsigned)src_ip, (unsigned)dst_ip, (unsigned)dp);
            netbuf_free(nb);
            return;
        }
    }

    const void *data = nb->data + UDP_HDR_LEN;
    uint32_t dlen = ulen - UDP_HDR_LEN;

    struct netns *ns = nif->ns;
    if (!ns) ns = netns_default();

    udp_recv_cb cb = 0;
    void *arg = 0;

    if (ns) {
        uint64_t fl;
        spin_lock_irqsave(&ns->udp_lock, &fl);
        for (int i = 0; i < UDP_MAX_BINDINGS; ++i) {
            struct udp_binding *b = ns->udp_binds[i];
            if (!b || !b->used) continue;
            if (b->local_port == dp &&
                (b->local_ip == 0 || b->local_ip == dst_ip)) {
                cb = b->cb; arg = b->arg; break;
            }
        }
        spin_unlock_irqrestore(&ns->udp_lock, fl);
    }

    if (cb) cb(nif, src_ip, sp, dp, data, dlen, arg);
    else serial_printf("[UDP] no binding for port %u\n", (unsigned)dp);

    netbuf_free(nb);
}

int udp_send(struct netif *nif, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             const void *payload, uint32_t len)
{
    uint32_t total = UDP_HDR_LEN + len;
    struct netbuf *nb = netbuf_alloc(nif->ns, total);
    if (!nb) return -1;

    struct udp_hdr *h = (struct udp_hdr *)nb->data;
    h->src_port = net_htons(src_port);
    h->dst_port = net_htons(dst_port);
    h->length   = net_htons((uint16_t)total);
    h->checksum = 0;

    if (len) {
        const uint8_t *p = (const uint8_t *)payload;
        uint8_t *q = (uint8_t *)(h + 1);
        for (uint32_t i = 0; i < len; ++i) q[i] = p[i];
    }
    nb->len = total;

    uint16_t c = udp_compute_checksum(nif->ip, dst_ip, nb->data, total);
    if (c == 0) c = 0xFFFF;
    h->checksum = net_htons(c);

    if (nif->ns && fw_hook(nif->ns, FW_DIR_OUT, FW_PROTO_UDP,
                           nif->ip, dst_ip, src_port, dst_port, 0) != 0) {
        netbuf_free(nb); return -1;
    }

    return ipv4_send(nif, dst_ip, IP_PROTO_UDP, nb);
}
/*===OmniBridgeOs/kernel/arch/x64/net/udp.c 结束===*/