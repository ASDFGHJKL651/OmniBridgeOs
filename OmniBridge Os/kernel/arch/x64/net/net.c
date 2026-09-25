/*===OmniBridgeOs/kernel/arch/x64/net/net.c===*/
#include "net.h"
#include "netbuf.h"
#include "netns.h"
#include "route.h"
#include "firewall.h"
#include "pci.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "socket.h"
#include "dhcp.h"
#include "dns.h"
#include "loopback.h"
#include "csprng.h"
#include "virtio_net.h"
#include "serial.h"

static int g_net_inited = 0;
static int g_net_has_eth = 0;

/* ============================================================
 * 初始化
 * ============================================================ */

void net_init(void)
{
    if (g_net_inited) return;
    g_net_inited = 1;

    serial_printf("[NET] init\n");

    netbuf_init();
    netns_init();
    route_init();
    fw_init();
    pci_init();
    ethernet_init();
    arp_init();
    ipv4_init();
    ipv6_init();
    icmp_init();
    udp_init();
    tcp_init();
    net_socket_init();
    dhcp_init();
    dns_init();
    csprng_init();

    struct netns *ns = netns_default();
    loopback_init(ns);

    /* 枚举 VirtIO 网卡 */
    if (virtio_net_init(ns) == 0) {
        g_net_has_eth = 1;

        /* ★ 第 18A 步修复：在等待 DHCP 期间必须主动轮询 VirtIO 队列，
         *   否则接收环永远不会被消费，DHCP OFFER 无法送达协议栈。 */
        struct netif *eth = ns->ifaces[ns->iface_count - 1];
        if (dhcp_start(eth) != 0) {
            /* dhcp_start 内部已尝试 6 轮，这里不再重复 */
            serial_printf("[NET] WARN: DHCP timeout; "
                        "falling back to static 10.0.2.15/24\n");
            eth->ip      = 0x0A00020Fu;
            eth->mask    = 0xFFFFFF00u;
            eth->gateway = 0x0A000202u;
            eth->dns[0]  = 0x0A000203u;
            route_add(ns, 0x0A000200u, 0xFFFFFF00u, 0, eth, 0);
            route_add(ns, 0, 0, 0x0A000202u, eth,
                    RT_FLAG_GATEWAY | RT_FLAG_DEFAULT);
        } else {
            serial_printf("[NET] DHCP succeeded: ip=%u.%u.%u.%u\n",
                        (unsigned)((eth->ip >> 24) & 0xFF),
                        (unsigned)((eth->ip >> 16) & 0xFF),
                        (unsigned)((eth->ip >> 8) & 0xFF),
                        (unsigned)(eth->ip & 0xFF));
        }
    } else {
        serial_printf("[NET] no network device; loopback only\n");
    }
}

/* ============================================================
 * 定时器轮询入口（★ 第 18A 步新增）
 *
 * 语义（人工必须审查）：
 *   - 该函数被 sched_tick() 与空闲线程调用，频率 ≈ 100Hz。
 *   - 遍历默认命名空间的所有接口并调用 poll（VirtIO 为轮询接收）。
 *   - 驱动 TCP 层的重传/超时定时器。
 *   - 沙盒命名空间中的接口目前无 poll 驱动，不在此处处理；
 *     沙盒进程发出的包已被 fw_hook() 直接拒绝，不产生流量。
 * ============================================================ */
void net_tick(void)
{
    if (!g_net_inited) return;

    struct netns *ns = netns_default();
    if (!ns) return;

    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    uint32_t n = ns->iface_count;
    /* 复制到栈上以避免在 poll 期间长时间持锁（ifaces 数组最多 8 个） */
    struct netif *local[8];
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n && cnt < 8; ++i) {
        local[cnt++] = ns->ifaces[i];
    }
    spin_unlock_irqrestore(&ns->lock, fl);

    for (uint32_t i = 0; i < cnt; ++i) {
        struct netif *nif = local[i];
        if (nif && nif->poll) nif->poll(nif);
    }

    /* TCP 重传/超时定时器 */
    tcp_tick();
}

/* ============================================================
 * 路由查找
 * ============================================================ */

struct netif *net_route_lookup(struct netns *ns, uint32_t dst_ip)
{
    if (!ns) ns = netns_default();

    /* 优先 loopback（127.0.0.0/8） */
    struct netif *lo = loopback_iface(ns);
    if (lo && (dst_ip >> 24) == 127) return lo;

    struct route_entry *rt = 0;
    if (route_lookup(ns, dst_ip, &rt) == 0 && rt && rt->nif) return rt->nif;

    /* 回退：第一个非 loopback 接口 */
    for (uint32_t i = 0; i < ns->iface_count; ++i) {
        if (ns->ifaces[i] && ns->ifaces[i]->up &&
            ns->ifaces[i] != lo) return ns->ifaces[i];
    }
    return lo;
}

/* ============================================================
 * 收发转发
 * ============================================================ */

int net_tx(struct netif *nif, struct netbuf *nb)
{
    if (!nif || !nb || !nif->xmit) return -1;
    return nif->xmit(nif, nb);
}

void net_rx(struct netif *nif, struct netbuf *nb)
{
    if (!nif || !nb) return;
    ethernet_rx(nif, nb);
}

/* ============================================================
 * 权限辅助
 * ============================================================ */

int net_check_task_access(struct task_t *cur)
{
    if (!cur) return OB_EPERM;
    if (cur->sandbox_flags != 0) return OB_ENETUNREACH;
    return 0;
}

/* ============================================================
 * 调试输出
 * ============================================================ */

void net_dump_ifaces(void)
{
    struct netns *ns = netns_default();
    serial_printf("[NET] interfaces:\n");
    for (uint32_t i = 0; i < ns->iface_count; ++i) {
        struct netif *nif = ns->ifaces[i];
        if (!nif) continue;
        serial_printf("  %s: mac=%02x:%02x:%02x:%02x:%02x:%02x ip=%u.%u.%u.%u "
                      "mask=%u.%u.%u.%u gw=%u.%u.%u.%u up=%u\n",
                      nif->name,
                      nif->mac[0], nif->mac[1], nif->mac[2],
                      nif->mac[3], nif->mac[4], nif->mac[5],
                      (unsigned)((nif->ip >> 24) & 0xFF),
                      (unsigned)((nif->ip >> 16) & 0xFF),
                      (unsigned)((nif->ip >> 8) & 0xFF),
                      (unsigned)(nif->ip & 0xFF),
                      (unsigned)((nif->mask >> 24) & 0xFF),
                      (unsigned)((nif->mask >> 16) & 0xFF),
                      (unsigned)((nif->mask >> 8) & 0xFF),
                      (unsigned)(nif->mask & 0xFF),
                      (unsigned)((nif->gateway >> 24) & 0xFF),
                      (unsigned)((nif->gateway >> 16) & 0xFF),
                      (unsigned)((nif->gateway >> 8) & 0xFF),
                      (unsigned)(nif->gateway & 0xFF),
                      (unsigned)nif->up);
    }
}

int net_has_eth(void) { return g_net_has_eth; }
/*===OmniBridgeOs/kernel/arch/x64/net/net.c 结束===*/