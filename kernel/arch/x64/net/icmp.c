/*===OmniBridgeOs/kernel/arch/x64/net/icmp.c===*/
#include "icmp.h"
#include "ipv4.h"
#include "csprng.h"
#include "serial.h"
#include "spinlock.h"

static spinlock_t g_ping_lock = SPINLOCK_INIT;
static volatile uint32_t g_ping_seen = 0;
static volatile uint16_t g_ping_id = 0;
static volatile uint16_t g_ping_seq = 0;

void icmp_init(void)
{
    serial_printf("[ICMP] init: echo request/reply\n");
}

void icmp_rx(struct netif *nif, uint32_t src_ip, struct netbuf *nb)
{
    if (nb->len < sizeof(struct icmp_hdr)) { netbuf_free(nb); return; }

    /* ★ 接收侧校验（含校验和字段整体反码和应为 0） */
    uint16_t c = ip_checksum(nb->data, nb->len);
    if (c != 0) {
        serial_printf("[ICMP] bad checksum (calc=0x%04x)\n", (unsigned)c);
        netbuf_free(nb); return;
    }

    struct icmp_hdr *h = (struct icmp_hdr *)nb->data;

    if (h->type == ICMP_ECHO_REQUEST) {
        uint16_t id  = net_ntohs(h->id);
        uint16_t seq = net_ntohs(h->seq);

        struct netbuf *r = netbuf_alloc(nif->ns, nb->len);
        if (!r) { netbuf_free(nb); return; }
        for (uint32_t i = 0; i < nb->len; ++i) r->data[i] = nb->data[i];
        r->len = nb->len;

        struct icmp_hdr *rh = (struct icmp_hdr *)r->data;
        rh->type = ICMP_ECHO_REPLY;
        rh->code = 0;
        rh->checksum = 0;
        rh->id  = net_htons(id);
        rh->seq = net_htons(seq);
        uint16_t rc = ip_checksum(r->data, r->len);
        rh->checksum = net_htons(rc);   /* ★ 字节序修复 */
        ipv4_send(nif, src_ip, IP_PROTO_ICMP, r);
    } else if (h->type == ICMP_ECHO_REPLY) {
        uint16_t id  = net_ntohs(h->id);
        uint16_t seq = net_ntohs(h->seq);
        if (id == g_ping_id && seq == g_ping_seq) {
            g_ping_seen = 1;
            serial_printf("[ICMP] reply from 0x%08x id=%u seq=%u\n",
                          (unsigned)src_ip, (unsigned)id, (unsigned)seq);
        }
    }
    netbuf_free(nb);
}

int icmp_send_echo(struct netif *nif, uint32_t dst_ip,
                   uint16_t id, uint16_t seq,
                   const void *payload, uint32_t payload_len)
{
    uint32_t total = (uint32_t)sizeof(struct icmp_hdr) + payload_len;
    struct netbuf *nb = netbuf_alloc(nif->ns, total);
    if (!nb) return -1;

    struct icmp_hdr *h = (struct icmp_hdr *)nb->data;
    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->checksum = 0;
    h->id  = net_htons(id);
    h->seq = net_htons(seq);
    if (payload && payload_len) {
        const uint8_t *p = (const uint8_t *)payload;
        uint8_t *q = (uint8_t *)(h + 1);
        for (uint32_t i = 0; i < payload_len; ++i) q[i] = p[i];
    }
    nb->len = total;

    uint16_t c = ip_checksum(nb->data, total);
    h->checksum = net_htons(c);   /* ★ 字节序修复 */
    return ipv4_send(nif, dst_ip, IP_PROTO_ICMP, nb);
}

int icmp_ping(struct netif *nif, uint32_t dst_ip,
              uint32_t payload_len, uint32_t timeout_ticks)
{
    if (!nif) return -1;
    uint64_t fl;
    spin_lock_irqsave(&g_ping_lock, &fl);
    g_ping_seen = 0;
    g_ping_id   = csprng_u16();
    g_ping_seq  = 1;
    spin_unlock_irqrestore(&g_ping_lock, fl);

    uint8_t fill[64];
    for (uint32_t i = 0; i < sizeof(fill); ++i) fill[i] = (uint8_t)i;
    if (payload_len > sizeof(fill)) payload_len = sizeof(fill);

    if (icmp_send_echo(nif, dst_ip, g_ping_id, g_ping_seq,
                       fill, payload_len) != 0) {
        serial_printf("[ICMP] ping: send failed\n");
        return -1;
    }

    for (uint32_t i = 0; i < timeout_ticks; ++i) {
        if (g_ping_seen) return 0;
        /* ★ 关键修复：轮询网卡以接收响应（真实网络必需） */
        if (nif->poll) nif->poll(nif);
        __asm__ __volatile__("pause" ::: "memory");
    }
    serial_printf("[ICMP] ping timeout for 0x%08x\n", (unsigned)dst_ip);
    return -1;
}
/*===OmniBridgeOs/kernel/arch/x64/net/icmp.c 结束===*/