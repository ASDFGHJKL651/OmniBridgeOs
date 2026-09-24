/*===OmniBridgeOs/kernel/arch/x64/net/ipv6.c===*/
#include "ipv6.h"
#include "ethernet.h"
#include "loopback.h"
#include "netbuf.h"
#include "net.h"
#include "csprng.h"
#include "serial.h"
#include "spinlock.h"

/* ---------- 常量地址 ---------- */
const uint8_t ipv6_addr_unspecified[16] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
};
const uint8_t ipv6_addr_loopback[16] = {
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1
};
const uint8_t ipv6_addr_all_nodes_mc[16] = {
    0xff,0x02,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1
};

/* ---------- ping 状态 ---------- */
static spinlock_t g_ping_lock = SPINLOCK_INIT;
static volatile uint32_t g_ping_seen = 0;
static volatile uint16_t g_ping_id = 0;
static volatile uint16_t g_ping_seq = 0;

void ipv6_init(void)
{
    spin_lock_init(&g_ping_lock);
    serial_printf("[IPv6] init: hdr=40B, ::1 loopback, ICMPv6 echo\n");
    serial_printf("[IPv6] WARN: NDP/SLAAC/RA not implemented in step 18A\n");
}

/* ============================================================
 * 地址辅助
 * ============================================================ */

int ipv6_addr_eq(const uint8_t a[16], const uint8_t b[16])
{
    for (int i = 0; i < 16; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

int ipv6_is_loopback(const uint8_t addr[16])
{
    for (int i = 0; i < 15; ++i) if (addr[i] != 0) return 0;
    return addr[15] == 1;
}

int ipv6_is_unspecified(const uint8_t addr[16])
{
    for (int i = 0; i < 16; ++i) if (addr[i] != 0) return 0;
    return 1;
}

int ipv6_is_multicast(const uint8_t addr[16])
{
    return (addr[0] == 0xFF);
}

void ipv6_addr_copy(uint8_t dst[16], const uint8_t src[16])
{
    for (int i = 0; i < 16; ++i) dst[i] = src[i];
}

void ipv6_addr_zero(uint8_t out[16])
{
    for (int i = 0; i < 16; ++i) out[i] = 0;
}

void ipv6_addr_from_ipv4(uint8_t out[16], uint32_t ipv4_host_order)
{
    for (int i = 0; i < 10; ++i) out[i] = 0;
    out[10] = 0xFF;
    out[11] = 0xFF;
    out[12] = (uint8_t)((ipv4_host_order >> 24) & 0xFF);
    out[13] = (uint8_t)((ipv4_host_order >> 16) & 0xFF);
    out[14] = (uint8_t)((ipv4_host_order >> 8) & 0xFF);
    out[15] = (uint8_t)(ipv4_host_order & 0xFF);
}

/* ============================================================
 * 校验和（IPv6 伪首部 + 载荷）
 *
 * 返回值语义：与 ip_checksum 一致 —— 返回值本身是"大端序的 16 位值"。
 * 写入 IPv6/ICMPv6 头部的 checksum 字段时，必须用 net_htons() 转换，
 * 否则小端机器上会按反序存储，接收方按大端读取就得到错误值。
 * ============================================================ */
uint16_t ipv6_checksum_pseudo(const uint8_t src[16], const uint8_t dst[16],
                              uint8_t next_hdr, uint32_t payload_len,
                              const void *payload)
{
    uint32_t sum = 0;

    for (int i = 0; i < 16; i += 2) {
        sum += ((uint32_t)src[i] << 8) | src[i + 1];
    }
    for (int i = 0; i < 16; i += 2) {
        sum += ((uint32_t)dst[i] << 8) | dst[i + 1];
    }
    sum += (payload_len >> 16) & 0xFFFFu;
    sum += payload_len & 0xFFFFu;
    sum += (uint32_t)next_hdr;

    const uint8_t *p = (const uint8_t *)payload;
    uint32_t n = payload_len;
    while (n > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; n -= 2;
    }
    if (n) sum += (uint32_t)p[0] << 8;

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

/* ============================================================
 * 出接口源地址选择（本步简化）
 * ============================================================ */
static void pick_src_v6(const uint8_t dst[16], uint8_t src_out[16])
{
    if (ipv6_is_loopback(dst)) {
        for (int i = 0; i < 16; ++i) src_out[i] = 0;
        src_out[15] = 1;
    } else {
        ipv6_addr_zero(src_out);
    }
}

/* ============================================================
 * 发送
 * ============================================================ */

int ipv6_send(struct netif *nif, const uint8_t dst[16],
              uint8_t next_hdr, struct netbuf *payload)
{
    if (!nif || !payload) { netbuf_free(payload); return -1; }

    struct netns *ns = nif->ns;

    struct netif *onif = nif;
    if (ipv6_is_loopback(dst)) {
        onif = loopback_iface(ns);
    } else if (ipv6_is_multicast(dst)) {
        serial_printf("[IPv6] send: multicast dst not supported in 18A\n");
        netbuf_free(payload);
        return -1;
    } else {
        serial_printf("[IPv6] send: external dst not supported in 18A\n");
        netbuf_free(payload);
        return -1;
    }

    uint32_t plen = payload->len;
    if (netbuf_push(payload, IPV6_HDR_LEN) != 0) {
        netbuf_free(payload);
        return -1;
    }
    struct ipv6_hdr *h = (struct ipv6_hdr *)payload->data;
    h->ver_tc_fl   = net_htonl(0x60000000u);
    h->payload_len = net_htons((uint16_t)plen);
    h->next_hdr    = next_hdr;
    h->hop_limit   = 64;

    uint8_t src[16];
    pick_src_v6(dst, src);
    for (int i = 0; i < 16; ++i) h->src[i] = src[i];
    for (int i = 0; i < 16; ++i) h->dst[i] = dst[i];

    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};

    return ethernet_send(onif, mac, ETH_TYPE_IPV6, payload);
}

/* ============================================================
 * 接收
 * ============================================================ */

void ipv6_rx(struct netif *nif, struct netbuf *nb)
{
    if (nb->len < IPV6_HDR_LEN) { netbuf_free(nb); return; }

    struct ipv6_hdr *h = (struct ipv6_hdr *)nb->data;
    uint32_t vtf = net_ntohl(h->ver_tc_fl);
    uint32_t ver = (vtf >> 28) & 0x0Fu;
    if (ver != 6) {
        serial_printf("[IPv6] rx: version=%u, dropped\n", (unsigned)ver);
        netbuf_free(nb);
        return;
    }

    uint16_t plen = net_ntohs(h->payload_len);
    uint8_t next_hdr = h->next_hdr;

    if ((uint32_t)plen > nb->len - IPV6_HDR_LEN) {
        serial_printf("[IPv6] rx: payload_len=%u > avail=%u, dropped\n",
                      (unsigned)plen,
                      (unsigned)(nb->len - IPV6_HDR_LEN));
        netbuf_free(nb);
        return;
    }

    uint8_t src[16], dst[16];
    for (int i = 0; i < 16; ++i) { src[i] = h->src[i]; dst[i] = h->dst[i]; }

    netbuf_pull(nb, IPV6_HDR_LEN);
    if (nb->len > plen) nb->len = plen;

    switch (next_hdr) {
    case IPPROTO_ICMPV6:
        icmpv6_rx(nif, src, dst, nb);
        return;

    case IPPROTO_HOPOPTS:
    case IPPROTO_FRAGMENT:
        serial_printf("[IPv6] rx: extension hdr=%u not supported in 18A\n",
                      (unsigned)next_hdr);
        netbuf_free(nb);
        return;

    case IPPROTO_UDPV6:
    case IPPROTO_TCPV6:
        serial_printf("[IPv6] rx: next_hdr=%u (UDP/TCP over v6) "
                      "not supported in 18A\n", (unsigned)next_hdr);
        netbuf_free(nb);
        return;

    default:
        serial_printf("[IPv6] rx: unknown next_hdr=%u, dropped\n",
                      (unsigned)next_hdr);
        netbuf_free(nb);
        return;
    }
}

/* ============================================================
 * ICMPv6（Echo 路径）
 *
 * ★ 本轮修复要点：
 *   1) 发送端：校验和写入内存前必须 net_htons()。
 *      原代码 `h->checksum = ipv6_checksum_pseudo(...)` 在小端机器上
 *      把大端值按小端存储，接收方按大端读取得到反序字节，导致日志中
 *      "[ICMPv6] rx: bad checksum (calc=... in=...)"。
 *   2) 接收端：验证方式改为"整个包（含已写入的校验和字段）的带伪首部
 *      反码和必须为 0"，这是 RFC 8200 §8.1 定义的验证方式。
 *      原代码把"重算结果"与"包中字段"直接比较，语义混淆。
 * ============================================================ */

void icmpv6_rx(struct netif *nif, const uint8_t src[16],
               const uint8_t dst[16], struct netbuf *nb)
{
    if (nb->len < sizeof(struct icmpv6_hdr)) { netbuf_free(nb); return; }

    struct icmpv6_hdr *h = (struct icmpv6_hdr *)nb->data;

    /*
     * RFC 8200 §8.1 验证：含校验和字段的完整包，其 IPv6 伪首部
     * 覆盖的反码和应为 0。
     */
    uint16_t cks_calc = ipv6_checksum_pseudo(src, dst, IPPROTO_ICMPV6,
                                             nb->len, nb->data);
    if (cks_calc != 0) {
        serial_printf("[ICMPv6] rx: bad checksum (verify=0x%04x "
                      "raw=0x%04x)\n",
                      (unsigned)cks_calc,
                      (unsigned)net_ntohs(h->checksum));
        netbuf_free(nb);
        return;
    }

    if (h->type == ICMPV6_ECHO_REQUEST) {
        uint16_t id  = net_ntohs(h->id);
        uint16_t seq = net_ntohs(h->seq);

        struct netbuf *r = netbuf_alloc(nif->ns, nb->len);
        if (!r) { netbuf_free(nb); return; }
        for (uint32_t i = 0; i < nb->len; ++i) r->data[i] = nb->data[i];
        r->len = nb->len;

        struct icmpv6_hdr *rh = (struct icmpv6_hdr *)r->data;
        rh->type     = ICMPV6_ECHO_REPLY;
        rh->code     = 0;
        rh->checksum = 0;                 /* 先清零再计算 */
        rh->id       = net_htons(id);
        rh->seq      = net_htons(seq);

        /*
         * Reply 的伪首部：源 = 原始包的 dst（本机），
         * 目的 = 原始包的 src（发起方）。
         */
        uint16_t cks = ipv6_checksum_pseudo(dst, src, IPPROTO_ICMPV6,
                                            r->len, r->data);
        /* ★ 关键修复：以网络字节序写入 */
        rh->checksum = net_htons(cks);

        ipv6_send(nif, src, IPPROTO_ICMPV6, r);
    } else if (h->type == ICMPV6_ECHO_REPLY) {
        uint16_t id  = net_ntohs(h->id);
        uint16_t seq = net_ntohs(h->seq);
        if (id == g_ping_id && seq == g_ping_seq) {
            g_ping_seen = 1;
            serial_printf("[ICMPv6] reply id=%u seq=%u\n",
                          (unsigned)id, (unsigned)seq);
        }
    } else if (h->type == 133 /* RS */ ||
               h->type == 134 /* RA */ ||
               h->type == 135 /* NS */ ||
               h->type == 136 /* NA */) {
        serial_printf("[ICMPv6] rx: NDP type=%u not implemented in 18A\n",
                      (unsigned)h->type);
    } else {
        serial_printf("[ICMPv6] rx: type=%u ignored\n", (unsigned)h->type);
    }

    netbuf_free(nb);
}

int icmpv6_send_echo(struct netif *nif, const uint8_t dst[16],
                     uint16_t id, uint16_t seq,
                     const void *payload, uint32_t payload_len)
{
    uint32_t total = (uint32_t)sizeof(struct icmpv6_hdr) + payload_len;
    struct netbuf *nb = netbuf_alloc(nif->ns, total);
    if (!nb) return -1;

    struct icmpv6_hdr *h = (struct icmpv6_hdr *)nb->data;
    h->type     = ICMPV6_ECHO_REQUEST;
    h->code     = 0;
    h->checksum = 0;                     /* 先清零再计算 */
    h->id       = net_htons(id);
    h->seq      = net_htons(seq);

    if (payload && payload_len) {
        const uint8_t *p = (const uint8_t *)payload;
        uint8_t *q = (uint8_t *)(h + 1);
        for (uint32_t i = 0; i < payload_len; ++i) q[i] = p[i];
    }
    nb->len = total;

    uint8_t src[16];
    pick_src_v6(dst, src);

    uint16_t cks = ipv6_checksum_pseudo(src, dst, IPPROTO_ICMPV6,
                                        total, nb->data);
    /* ★ 关键修复：以网络字节序写入 */
    h->checksum = net_htons(cks);

    return ipv6_send(nif, dst, IPPROTO_ICMPV6, nb);
}

int ipv6_ping(struct netif *nif, const uint8_t dst[16], uint32_t timeout_ticks)
{
    if (!nif) return -1;

    uint64_t fl;
    spin_lock_irqsave(&g_ping_lock, &fl);
    g_ping_seen = 0;
    g_ping_id   = csprng_u16();
    g_ping_seq  = 1;
    spin_unlock_irqrestore(&g_ping_lock, fl);

    uint8_t fill[32];
    for (uint32_t i = 0; i < sizeof(fill); ++i) fill[i] = (uint8_t)i;

    if (icmpv6_send_echo(nif, dst, g_ping_id, g_ping_seq,
                         fill, sizeof(fill)) != 0) {
        serial_printf("[ICMPv6] ping: send failed\n");
        return -1;
    }

    for (uint32_t i = 0; i < timeout_ticks; ++i) {
        if (g_ping_seen) return 0;
        /* ★ 关键修复：轮询网卡以接收响应 */
        if (nif->poll) nif->poll(nif);
        __asm__ __volatile__("pause" ::: "memory");
    }
    serial_printf("[ICMPv6] ping timeout\n");
    return -1;
}

/* ============================================================
 * 邻居发现（NDP）—— 本步为桩
 *
 * 后继步骤应实现：
 *   1) NDP 缓存（IPv6 地址 → MAC）
 *   2) 发送 Neighbor Solicitation（ICMPv6 type=135）
 *   3) 处理 Neighbor Advertisement（type=136）
 *   4) 覆盖 ::1 → 00:00:00:00:00:00 的特殊情况
 *   5) 路由器发现（RS/RA）
 * ============================================================ */
int ipv6_ndp_resolve(struct netif *nif, const uint8_t dst[16],
                     uint8_t out_mac[6])
{
    (void)nif; (void)dst; (void)out_mac;
    serial_printf("[IPv6-NDP] WARN: ndp_resolve not implemented in 18A\n");
    return -1;
}
/*===OmniBridgeOs/kernel/arch/x64/net/ipv6.c 结束===*/