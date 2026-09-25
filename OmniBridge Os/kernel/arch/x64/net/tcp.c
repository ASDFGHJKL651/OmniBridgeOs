/*===OmniBridgeOs/kernel/arch/x64/net/tcp.c===*/
#include "tcp.h"
#include "ipv4.h"
#include "udp.h"
#include "csprng.h"
#include "firewall.h"
#include "netns.h"
#include "serial.h"
#include "spinlock.h"

static struct tcp_conn g_conns[TCP_MAX_CONNS];
static spinlock_t      g_lock = SPINLOCK_INIT;
static uint64_t        g_tick = 0;

/*
 * tcp_output 重入保护：
 *   loopback 是同步的，tcp_send_segment 内部会通过
 *   ipv4_send → lo_xmit → ethernet_rx → ipv4_rx → tcp_rx 立刻把包送
 *   到对端。对端的 tcp_rx 末尾又会调用 tcp_output(c)。
 *   g_output_depth 保证同一时刻只有一个线程真正执行 tcp_output 主体。
 */
static int g_output_depth = 0;
#define TCP_OUTPUT_MAX_DEPTH 2

void tcp_congestion_init(struct tcp_conn *c)
{
    if (!c) return;
    c->cwnd      = 1;
    c->ssthresh  = 65535;
    c->rto_ticks = TCP_RETX_TICKS;
}

void tcp_congestion_on_ack(struct tcp_conn *c, uint32_t acked)
{
    if (!c || acked == 0) return;
    if (c->cwnd < c->ssthresh) {
        c->cwnd += acked;
        if (c->cwnd > c->ssthresh) c->cwnd = c->ssthresh;
    } else {
        c->cwnd++;
    }
}

void tcp_congestion_on_loss(struct tcp_conn *c)
{
    if (!c) return;
    c->ssthresh = c->cwnd / 2;
    if (c->ssthresh < 2) c->ssthresh = 2;
    c->cwnd = 1;
}

void tcp_init(void)
{
    spin_lock_init(&g_lock);
    for (int i = 0; i < TCP_MAX_CONNS; ++i) g_conns[i].used = 0;
    serial_printf("[TCP] init: max_conns=%d\n", TCP_MAX_CONNS);
}

static struct tcp_conn *tcp_alloc(struct netns *ns)
{
    for (int i = 0; i < TCP_MAX_CONNS; ++i) {
        if (!g_conns[i].used) {
            struct tcp_conn *c = &g_conns[i];
            uint8_t *p = (uint8_t *)c;
            for (unsigned k = 0; k < sizeof(*c); ++k) p[k] = 0;
            c->used = 1;
            c->state = TCP_CLOSED;
            c->snd_wnd = 4096;
            c->rcv_wnd = (uint16_t)TCP_RX_BUF_SIZE;
            c->ns = ns ? ns : netns_default();
            tcp_congestion_init(c);
            return c;
        }
    }
    return 0;
}

static void tcp_free(struct tcp_conn *c)
{
    if (!c) return;
    c->used = 0;
    c->state = TCP_CLOSED;
}

void tcp_set_callbacks(struct tcp_conn *c,
                       void (*on_accept)(struct tcp_conn *, void *),
                       void (*on_recv)(struct tcp_conn *, void *),
                       void *arg)
{
    if (!c) return;
    c->on_accept = on_accept;
    c->on_recv   = on_recv;
    c->cb_arg    = arg;
}

static int tcp_send_segment(struct tcp_conn *c, struct netif *nif,
                            uint32_t seq, uint32_t ack, uint8_t flags,
                            const void *data, uint32_t dlen)
{
    uint32_t total = TCP_HDR_MIN + dlen;
    struct netbuf *nb = netbuf_alloc(nif->ns, total);
    if (!nb) return -1;

    struct tcp_hdr *h = (struct tcp_hdr *)nb->data;
    h->src_port = net_htons(c->local_port);
    h->dst_port = net_htons(c->remote_port);
    h->seq      = net_htonl(seq);
    h->ack      = net_htonl(ack);
    h->data_off = (uint8_t)((TCP_HDR_MIN / 4) << 4);
    h->flags    = flags;
    h->window   = net_htons(c->rcv_wnd);
    h->checksum = 0;
    h->urgent   = 0;

    if (dlen) {
        uint8_t *q = (uint8_t *)(h + 1);
        const uint8_t *p = (const uint8_t *)data;
        for (uint32_t i = 0; i < dlen; ++i) q[i] = p[i];
    }
    nb->len = total;

    /*
     * ★ 修复（v11）：ip_checksum_pseudo 期望 host order 的 IP。
     *   之前传 net_htonl(c->local_ip) 会导致 TCP 伪首部校验和与
     *   真实网络包不匹配，外部连接必然失败。
     */
    uint16_t cks = ip_checksum_pseudo(
        c->local_ip, c->remote_ip,
        IP_PROTO_TCP, total, nb->data, total, 0, 0);
    h->checksum = net_htons(cks);

    if (nif->ns && fw_hook(nif->ns, FW_DIR_OUT, FW_PROTO_TCP,
                           c->local_ip, c->remote_ip,
                           c->local_port, c->remote_port, 0) != 0) {
        netbuf_free(nb);
        return -1;
    }
    return ipv4_send(nif, c->remote_ip, IP_PROTO_TCP, nb);
}

static int tcp_output(struct tcp_conn *c)
{
    if (!c || c->state != TCP_ESTABLISHED) return -1;

    if (g_output_depth >= TCP_OUTPUT_MAX_DEPTH) {
        return 0;
    }
    g_output_depth++;

    struct netif *nif = net_route_lookup(c->ns, c->remote_ip);
    if (!nif) {
        g_output_depth--;
        return -1;
    }

    for (uint32_t i = 0; i < TCP_TXQ_MAX; ++i) {
        struct tcp_tx_seg *s = &c->txq[i];
        if (!s->used || s->sent) continue;
        if (tcp_send_segment(c, nif, s->seq, c->rcv_nxt,
                             TCP_ACK | TCP_PSH, s->data, s->len) == 0) {
            s->sent = 1;
            s->last_tick = g_tick;
            if (s->seq + s->len > c->snd_nxt)
                c->snd_nxt = s->seq + s->len;
        } else {
            g_output_depth--;
            return -1;
        }
    }

    g_output_depth--;
    return 0;
}

struct tcp_conn *tcp_listen(struct netns *ns, uint32_t local_ip,
                            uint16_t local_port)
{
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    struct tcp_conn *c = tcp_alloc(ns);
    if (c) {
        c->state      = TCP_LISTEN;
        c->local_ip   = local_ip;
        c->local_port = local_port;
    }
    spin_unlock_irqrestore(&g_lock, fl);
    if (c) serial_printf("[TCP] listen 0x%08x:%u\n",
                         (unsigned)local_ip, (unsigned)local_port);
    return c;
}

struct tcp_conn *tcp_connect(struct netns *ns, uint32_t remote_ip,
                             uint16_t remote_port)
{
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    struct tcp_conn *c = tcp_alloc(ns);
    spin_unlock_irqrestore(&g_lock, fl);
    if (!c) return 0;

    c->local_ip    = 0;
    c->local_port  = udp_alloc_ephemeral();
    c->remote_ip   = remote_ip;
    c->remote_port = remote_port;
    c->snd_una     = csprng_u32();
    c->snd_nxt     = c->snd_una;
    c->rcv_nxt     = 0;
    c->state       = TCP_SYN_SENT;
    c->last_tick   = g_tick;
    c->retx_tick   = g_tick;
    tcp_congestion_init(c);

    struct netif *nif = net_route_lookup(c->ns ? c->ns : netns_default(),
                                         remote_ip);
    if (!nif) { tcp_free(c); return c; }
    c->local_ip = nif->ip;

    tcp_send_segment(c, nif, c->snd_una, 0, TCP_SYN, 0, 0);
    c->snd_nxt = c->snd_una + 1;
    serial_printf("[TCP] SYN -> 0x%08x:%u from port %u\n",
                  (unsigned)remote_ip, (unsigned)remote_port,
                  (unsigned)c->local_port);
    return c;
}

int tcp_send_data(struct tcp_conn *c, const void *data, uint32_t len)
{
    if (!c || c->state != TCP_ESTABLISHED) return -1;
    if (!data && len > 0) return -1;
    if (len > TCP_TX_BUF_SIZE) len = TCP_TX_BUF_SIZE;

    uint32_t copied = 0;
    const uint8_t *p = (const uint8_t *)data;

    while (copied < len) {
        int slot = -1;
        for (uint32_t i = 0; i < TCP_TXQ_MAX; ++i) {
            if (!c->txq[i].used) { slot = (int)i; break; }
        }
        if (slot < 0) return copied > 0 ? (int)copied : -1;

        struct tcp_tx_seg *s = &c->txq[slot];
        uint32_t chunk = len - copied;
        if (chunk > TCP_TX_BUF_SIZE) chunk = TCP_TX_BUF_SIZE;

        s->used = 1;
        s->sent = 0;
        s->retx = 0;
        s->seq  = c->snd_nxt;
        s->len  = chunk;
        s->last_tick = g_tick;
        for (uint32_t i = 0; i < chunk; ++i) s->data[i] = p[copied + i];

        c->snd_nxt += chunk;
        copied += chunk;
        c->txq_count++;
    }

    (void)tcp_output(c);
    return (int)copied;
}

int tcp_recv_data(struct tcp_conn *c, void *buf, uint32_t max_len)
{
    if (!c) return -1;
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    uint32_t n = c->rx_len;
    if (n > max_len) n = max_len;
    uint8_t *d = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; ++i) d[i] = c->rx_buf[i];
    for (uint32_t i = n; i < c->rx_len; ++i) c->rx_buf[i - n] = c->rx_buf[i];
    c->rx_len -= n;
    spin_unlock_irqrestore(&g_lock, fl);
    return (int)n;
}

void tcp_close(struct tcp_conn *c)
{
    if (!c) return;
    struct netif *nif = net_route_lookup(c->ns, c->remote_ip);
    if (nif && c->state == TCP_ESTABLISHED) {
        tcp_send_segment(c, nif, c->snd_nxt, c->rcv_nxt,
                         TCP_FIN | TCP_ACK, 0, 0);
        c->snd_nxt++;
        c->state = TCP_FIN_WAIT_1;
    } else {
        tcp_free(c);
    }
}

static void tcp_process_ack(struct tcp_conn *c, uint32_t ack)
{
    if (ack > c->snd_una) {
        uint32_t acked = ack - c->snd_una;
        c->snd_una = ack;
        tcp_congestion_on_ack(c, acked);
        for (uint32_t i = 0; i < TCP_TXQ_MAX; ++i) {
            struct tcp_tx_seg *s = &c->txq[i];
            if (!s->used) continue;
            if (s->seq + s->len <= ack) {
                s->used = 0;
                if (c->txq_count > 0) c->txq_count--;
            }
        }
    }
}

static struct tcp_conn *tcp_find(struct netns *ns, uint16_t dst_port,
                                 uint16_t src_port, uint32_t src_ip)
{
    if (!ns) ns = netns_default();

    struct tcp_conn *listener = 0;

    for (int i = 0; i < TCP_MAX_CONNS; ++i) {
        struct tcp_conn *e = &g_conns[i];
        if (!e->used) continue;
        if (ns && e->ns != ns) continue;

        if (e->state == TCP_LISTEN) {
            if (!listener && e->local_port == dst_port) listener = e;
            continue;
        }

        if (e->local_port  == dst_port &&
            e->remote_port == src_port &&
            e->remote_ip   == src_ip) {
            return e;
        }
    }

    return listener;
}

void tcp_rx(struct netif *nif, uint32_t src_ip, uint32_t dst_ip,
            struct netbuf *nb)
{
    if (nb->len < TCP_HDR_MIN) { netbuf_free(nb); return; }
    struct tcp_hdr *h = (struct tcp_hdr *)nb->data;

    uint16_t sp = net_ntohs(h->src_port);
    uint16_t dp = net_ntohs(h->dst_port);
    uint32_t seq = net_ntohl(h->seq);
    uint32_t ack = net_ntohl(h->ack);
    uint8_t  flg = h->flags;
    uint8_t  doff = (uint8_t)((h->data_off >> 4) & 0x0F);
    uint32_t hlen = (uint32_t)doff * 4;
    if (hlen < TCP_HDR_MIN || hlen > nb->len) { netbuf_free(nb); return; }

    const void *payload = nb->data + hlen;
    uint32_t plen = nb->len - hlen;

    struct netns *ns = nif->ns ? nif->ns : netns_default();

    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    struct tcp_conn *c = tcp_find(ns, dp, sp, src_ip);
    spin_unlock_irqrestore(&g_lock, fl);

    if (!c) {
        serial_printf("[TCP] no conn for %u->%u\n",
                      (unsigned)sp, (unsigned)dp);
        netbuf_free(nb);
        return;
    }

    if (c->state == TCP_LISTEN) {
        if (flg & TCP_SYN) {
            struct tcp_conn *child = tcp_alloc(ns);
            if (child) {
                child->local_ip    = dst_ip;
                child->remote_ip   = src_ip;
                child->local_port  = dp;
                child->remote_port = sp;
                child->snd_una     = csprng_u32();
                child->snd_nxt     = child->snd_una;
                child->rcv_nxt     = seq + 1;
                child->state       = TCP_SYN_RCVD;
                tcp_send_segment(child, nif, child->snd_una,
                                 child->rcv_nxt, TCP_SYN | TCP_ACK, 0, 0);
                child->snd_nxt++;
                if (child->on_accept) child->on_accept(child, child->cb_arg);
            }
        }
        netbuf_free(nb);
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((flg & TCP_SYN) && (flg & TCP_ACK)) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            tcp_send_segment(c, nif, c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
            c->state = TCP_ESTABLISHED;
            serial_printf("[TCP] ESTABLISHED with 0x%08x:%u\n",
                          (unsigned)src_ip, (unsigned)sp);
        } else if (flg & TCP_RST) {
            tcp_free(c);
        }
        netbuf_free(nb);
        return;
    }

    if (c->state == TCP_SYN_RCVD) {
        if (flg & TCP_ACK) {
            c->snd_una = ack;
            c->state   = TCP_ESTABLISHED;
            serial_printf("[TCP] ESTABLISHED (passive) port=%u\n",
                          (unsigned)c->local_port);
        }
        netbuf_free(nb);
        return;
    }

    if (c->state == TCP_ESTABLISHED) {
        if (flg & TCP_ACK) tcp_process_ack(c, ack);

        if (plen && seq == c->rcv_nxt) {
            uint64_t lf;
            spin_lock_irqsave(&g_lock, &lf);
            uint32_t space = TCP_RX_BUF_SIZE - c->rx_len;
            uint32_t take = plen < space ? plen : space;
            for (uint32_t i = 0; i < take; ++i)
                c->rx_buf[c->rx_len + i] = ((const uint8_t *)payload)[i];
            c->rx_len += take;
            spin_unlock_irqrestore(&g_lock, lf);
            c->rcv_nxt += plen;
            if (c->on_recv) c->on_recv(c, c->cb_arg);
        }

        if (plen) {
            tcp_send_segment(c, nif, c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
        }

        if (flg & TCP_FIN) {
            c->rcv_nxt++;
            tcp_send_segment(c, nif, c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
            c->state = TCP_CLOSE_WAIT;
        }

        tcp_output(c);
    } else if (c->state == TCP_FIN_WAIT_1) {
        if (flg & TCP_ACK) c->state = TCP_FIN_WAIT_2;
        if (flg & TCP_FIN) {
            c->rcv_nxt++;
            tcp_send_segment(c, nif, c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
            c->state = TCP_TIME_WAIT;
        }
    } else if (c->state == TCP_FIN_WAIT_2) {
        if (flg & TCP_FIN) {
            c->rcv_nxt++;
            tcp_send_segment(c, nif, c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
            c->state = TCP_TIME_WAIT;
        }
    } else if (c->state == TCP_CLOSE_WAIT) {
        if (flg & TCP_ACK) c->state = TCP_LAST_ACK;
    } else if (c->state == TCP_LAST_ACK) {
        if (flg & TCP_ACK) tcp_free(c);
    } else if (c->state == TCP_TIME_WAIT) {
        if (flg & TCP_FIN) {
            tcp_send_segment(c, nif, c->snd_nxt, c->rcv_nxt, TCP_ACK, 0, 0);
        }
    }

    netbuf_free(nb);
}

void tcp_tick(void)
{
    g_tick++;
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);

    for (int i = 0; i < TCP_MAX_CONNS; ++i) {
        struct tcp_conn *c = &g_conns[i];
        if (!c->used) continue;

        if (c->state == TCP_SYN_SENT &&
            (g_tick - c->retx_tick) > c->rto_ticks) {
            serial_printf("[TCP] SYN timeout conn=%d\n", i);
            tcp_free(c);
            continue;
        }

        if (c->state == TCP_ESTABLISHED) {
            for (uint32_t k = 0; k < TCP_TXQ_MAX; ++k) {
                struct tcp_tx_seg *s = &c->txq[k];
                if (!s->used || !s->sent) continue;
                if ((g_tick - s->last_tick) > c->rto_ticks) {
                    struct netif *nif = net_route_lookup(c->ns, c->remote_ip);
                    if (nif) {
                        tcp_send_segment(c, nif, s->seq, c->rcv_nxt,
                                         TCP_ACK | TCP_PSH, s->data, s->len);
                        s->last_tick = g_tick;
                        s->retx++;
                        if (s->retx > 5) tcp_congestion_on_loss(c);
                    }
                }
            }
        }

        if (c->state == TCP_FIN_WAIT_2 && (g_tick - c->last_tick) > 600) {
            tcp_free(c);
        }
        if (c->state == TCP_TIME_WAIT && (g_tick - c->last_tick) > 600) {
            tcp_free(c);
        }
    }
    spin_unlock_irqrestore(&g_lock, fl);
}
/*===OmniBridgeOs/kernel/arch/x64/net/tcp.c 结束===*/