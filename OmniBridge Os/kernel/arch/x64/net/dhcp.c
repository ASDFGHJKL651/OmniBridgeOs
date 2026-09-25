/*===OmniBridgeOs/kernel/arch/x64/net/dhcp.c===*/
/*
 * DHCP 客户端（第 18A 步）。
 *
 * v5 修复：
 *   1) dhcp_send_discover / dhcp_send_request 里临时 netbuf 现由
 *      调用方释放（原实现每轮泄漏 2 个，6 轮累计 12 个）；
 *   2) dhcp_start 等待窗口从 6×30×5000 pause（≈27ms）延长到
 *      6×100×50000 pause（≈600ms）。
 *
 * v6 补充：
 *   3) 每轮结束后调用 virtio_net_debug_dump()，把 TX/RX 队列的
 *      used->idx 进度直接打到串口 —— 用来精确定位"设备没收到 TX"
 *      还是"驱动看不到 RX"。
 */
#include "dhcp.h"
#include "udp.h"
#include "route.h"
#include "csprng.h"
#include "serial.h"
#include "virtio_net.h"     /* ★ v6：virtio_net_debug_dump */

#define DHCP_MAGIC 0x63825363u
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define DHCP_STATE_INIT     0
#define DHCP_STATE_DISCOVER 1
#define DHCP_STATE_REQUEST  2
#define DHCP_STATE_BOUND    3

struct dhcp_msg {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[64];
} __attribute__((packed));

static uint32_t g_xid = 0;
static volatile int g_done = 0;
static struct netif *g_nif = 0;
static uint32_t g_yiaddr = 0;
static uint32_t g_mask   = 0;
static uint32_t g_gw     = 0;
static uint32_t g_dns    = 0;
static volatile uint8_t g_state = DHCP_STATE_INIT;
static uint32_t g_server_id = 0;
static uint32_t g_requested_ip = 0;

static int dhcp_send_discover(struct netif *nif);
static int dhcp_send_request(struct netif *nif);

static uint32_t dhcp_opt_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void dhcp_apply_config(struct netif *nif)
{
    nif->ip      = g_yiaddr;
    nif->mask    = g_mask ? g_mask : 0xFFFFFF00u;
    nif->gateway = g_gw;
    nif->dns[0]  = g_dns;
    nif->dns[1]  = 0;

    route_add(nif->ns, nif->ip & nif->mask, nif->mask, 0, nif, 0);
    if (g_gw) {
        route_add(nif->ns, 0, 0, g_gw, nif,
                  RT_FLAG_GATEWAY | RT_FLAG_DEFAULT);
    }
}

static void dhcp_udp_cb(struct netif *nif, uint32_t src_ip,
                        uint16_t src_port, uint16_t dst_port,
                        const void *data, uint32_t len, void *arg)
{
    (void)src_ip; (void)src_port; (void)dst_port; (void)arg;
    if (len < sizeof(struct dhcp_msg)) return;
    const struct dhcp_msg *m = (const struct dhcp_msg *)data;
    if (net_ntohl(m->xid) != g_xid) return;
    if (m->op != 2) return;

    uint8_t msg_type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, server_id = 0;
    const uint8_t *opt = m->options;
    uint32_t off = 0;
    while (off < 64) {
        uint8_t code = opt[off++];
        if (code == 0) continue;
        if (code == 255) break;
        if (off >= 64) break;
        uint8_t ol = opt[off++];
        if (off + ol > 64) break;

        if (code == 53 && ol == 1) {
            msg_type = opt[off];
        } else if (code == 1 && ol == 4) {
            mask = dhcp_opt_u32(opt + off);
        } else if (code == 3 && ol == 4) {
            gw = dhcp_opt_u32(opt + off);
        } else if (code == 6 && ol >= 4) {
            dns = dhcp_opt_u32(opt + off);
        } else if (code == 54 && ol == 4) {
            server_id = dhcp_opt_u32(opt + off);
        }
        off += ol;
    }

    uint32_t yi = net_ntohl(m->yiaddr);
    if (yi == 0) return;

    if (msg_type == DHCP_OFFER && g_state == DHCP_STATE_DISCOVER) {
        g_yiaddr = yi;
        g_server_id = server_id;
        g_requested_ip = yi;
        g_mask = mask;
        g_gw = gw;
        g_dns = dns;
        g_state = DHCP_STATE_REQUEST;
        serial_printf("[DHCP] OFFER %u.%u.%u.%u server=%u.%u.%u.%u\n",
                      (unsigned)((yi >> 24) & 0xFF), (unsigned)((yi >> 16) & 0xFF),
                      (unsigned)((yi >> 8) & 0xFF),  (unsigned)(yi & 0xFF),
                      (unsigned)((server_id >> 24) & 0xFF), (unsigned)((server_id >> 16) & 0xFF),
                      (unsigned)((server_id >> 8) & 0xFF),  (unsigned)(server_id & 0xFF));
        dhcp_send_request(nif);
        return;
    }

    if (msg_type == DHCP_ACK && g_state == DHCP_STATE_REQUEST) {
        g_yiaddr = yi;
        g_mask = mask ? mask : g_mask;
        g_gw = gw ? gw : g_gw;
        g_dns = dns ? dns : g_dns;
        dhcp_apply_config(nif);
        g_done = 1;
        g_state = DHCP_STATE_BOUND;
        serial_printf("[DHCP] got IP %u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u dns=%u.%u.%u.%u\n",
                      (unsigned)((g_yiaddr >> 24) & 0xFF), (unsigned)((g_yiaddr >> 16) & 0xFF),
                      (unsigned)((g_yiaddr >> 8) & 0xFF),  (unsigned)(g_yiaddr & 0xFF),
                      (unsigned)((nif->mask >> 24) & 0xFF), (unsigned)((nif->mask >> 16) & 0xFF),
                      (unsigned)((nif->mask >> 8) & 0xFF),  (unsigned)(nif->mask & 0xFF),
                      (unsigned)((g_gw >> 24) & 0xFF), (unsigned)((g_gw >> 16) & 0xFF),
                      (unsigned)((g_gw >> 8) & 0xFF),  (unsigned)(g_gw & 0xFF),
                      (unsigned)((g_dns >> 24) & 0xFF), (unsigned)((g_dns >> 16) & 0xFF),
                      (unsigned)((g_dns >> 8) & 0xFF),  (unsigned)(g_dns & 0xFF));
    }
}

int dhcp_handle_offer_ack(struct netif *nif, const void *payload,
                          uint32_t len, uint8_t is_ack)
{
    (void)is_ack;
    dhcp_udp_cb(nif, 0, 67, 68, payload, len, 0);
    return 0;
}

static int dhcp_send_discover(struct netif *nif)
{
    struct netbuf *nb = netbuf_alloc(nif->ns, sizeof(struct dhcp_msg));
    if (!nb) return -1;
    uint8_t *p = nb->data;
    for (unsigned i = 0; i < sizeof(struct dhcp_msg); ++i) p[i] = 0;

    struct dhcp_msg *m = (struct dhcp_msg *)p;
    m->op    = 1;
    m->htype = 1;
    m->hlen  = 6;
    m->xid   = net_htonl(g_xid);
    m->flags = net_htons(0x8000);
    for (int i = 0; i < 6; ++i) m->chaddr[i] = nif->mac[i];
    m->magic = net_htonl(DHCP_MAGIC);
    m->options[0] = 53;
    m->options[1] = 1;
    m->options[2] = DHCP_DISCOVER;
    m->options[3] = 255;
    nb->len = sizeof(struct dhcp_msg);

    g_state = DHCP_STATE_DISCOVER;
    int rc = udp_send(nif, 0xFFFFFFFFu, 68, 67, nb->data, nb->len);
    netbuf_free(nb);                          /* ★ v5：调用方释放 */
    return rc;
}

static int dhcp_send_request(struct netif *nif)
{
    struct netbuf *nb = netbuf_alloc(nif->ns, sizeof(struct dhcp_msg));
    if (!nb) return -1;
    uint8_t *p = nb->data;
    for (unsigned i = 0; i < sizeof(struct dhcp_msg); ++i) p[i] = 0;

    struct dhcp_msg *m = (struct dhcp_msg *)p;
    m->op    = 1;
    m->htype = 1;
    m->hlen  = 6;
    m->xid   = net_htonl(g_xid);
    m->flags = net_htons(0x8000);
    for (int i = 0; i < 6; ++i) m->chaddr[i] = nif->mac[i];
    m->magic = net_htonl(DHCP_MAGIC);

    uint32_t o = 0;
    m->options[o++] = 53; m->options[o++] = 1; m->options[o++] = DHCP_REQUEST;
    m->options[o++] = 50; m->options[o++] = 4;
    m->options[o++] = (uint8_t)((g_requested_ip >> 24) & 0xFF);
    m->options[o++] = (uint8_t)((g_requested_ip >> 16) & 0xFF);
    m->options[o++] = (uint8_t)((g_requested_ip >> 8) & 0xFF);
    m->options[o++] = (uint8_t)(g_requested_ip & 0xFF);
    m->options[o++] = 54; m->options[o++] = 4;
    m->options[o++] = (uint8_t)((g_server_id >> 24) & 0xFF);
    m->options[o++] = (uint8_t)((g_server_id >> 16) & 0xFF);
    m->options[o++] = (uint8_t)((g_server_id >> 8) & 0xFF);
    m->options[o++] = (uint8_t)(g_server_id & 0xFF);
    m->options[o++] = 255;
    nb->len = sizeof(struct dhcp_msg);

    serial_printf("[DHCP] REQUEST %u.%u.%u.%u\n",
                  (unsigned)((g_requested_ip >> 24) & 0xFF),
                  (unsigned)((g_requested_ip >> 16) & 0xFF),
                  (unsigned)((g_requested_ip >> 8) & 0xFF),
                  (unsigned)(g_requested_ip & 0xFF));
    int rc = udp_send(nif, 0xFFFFFFFFu, 68, 67, nb->data, nb->len);
    netbuf_free(nb);                          /* ★ v5：调用方释放 */
    return rc;
}

void dhcp_init(void)
{
    serial_printf("[DHCP] init: client 0.0.0.0/68 -> broadcast/67\n");
}

/*
 * ★ v5：等待窗口参数。
 *
 *   旧参数：6 × 30 × 5000 pause ≈ 27ms。
 *   新参数：6 × 100 × 50000 pause ≈ 600ms。
 *
 *   配合 virtio_net.c 中 v6 的原子读修复，SLIRP 通常首轮即完成
 *   OFFER→REQUEST→ACK；这里的窗口是作为兜底，保证即使某个环节
 *   有延迟也能成功。
 */
#define DHCP_ROUNDS     6
#define DHCP_POLL_ITERS 100
#define DHCP_PAUSE_EACH 50000

int dhcp_start(struct netif *nif)
{
    if (!nif) return -1;
    g_nif = nif;
    g_done = 0;
    g_state = DHCP_STATE_INIT;
    g_server_id = 0;
    g_requested_ip = 0;
    g_xid = csprng_u32();

    serial_printf("[DHCP] starting, xid=0x%x mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  (unsigned)g_xid,
                  nif->mac[0], nif->mac[1], nif->mac[2],
                  nif->mac[3], nif->mac[4], nif->mac[5]);

    if (udp_bind(nif->ns, 68, 0, dhcp_udp_cb, 0) != 0) {
        serial_printf("[DHCP] udp_bind(68) failed\n");
        return -1;
    }

    /* 预轮询：让 virtio-net 设备把 RX avail ring 的第一批描述符
     * 处理完，确保后续 OFFER 到达时驱动侧有空闲描述符可用。 */
    if (nif->poll) {
        for (int i = 0; i < 4; ++i) {
            nif->poll(nif);
            for (volatile int j = 0; j < 10000; ++j) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }
    }

    for (int round = 0; round < DHCP_ROUNDS; ++round) {
        int rc = dhcp_send_discover(nif);
        serial_printf("[DHCP] DISCOVER sent (round=%d rc=%d)\n", round, rc);

        for (int i = 0; i < DHCP_POLL_ITERS && !g_done; ++i) {
            if (nif->poll) nif->poll(nif);
            for (volatile int j = 0; j < DHCP_PAUSE_EACH; ++j) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }

        /* ★ v6：诊断转储 —— 直接暴露 TX/RX 队列的 used->idx 推进 */
        virtio_net_debug_dump(nif);

        if (g_done) return 0;
    }

    serial_printf("[DHCP] timeout after %d rounds\n", DHCP_ROUNDS);
    return -1;
}

int dhcp_is_done(void) { return g_done; }
/*===OmniBridgeOs/kernel/arch/x64/net/dhcp.c 结束===*/