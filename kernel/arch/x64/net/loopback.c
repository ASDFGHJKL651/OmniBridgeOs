/*===OmniBridgeOs/kernel/arch/x64/net/loopback.c===*/
#include "loopback.h"
#include "ethernet.h"
#include "arp.h"
#include "route.h"
#include "kmalloc.h"
#include "serial.h"

static struct netif g_lo;

/*
 * ★ 第 18A 步修复：
 *   原实现的 lo_xmit 在调用 ethernet_rx() 之后又调用 netbuf_free()，
 *   导致 ethernet_rx → ipv4/icmp 的路径中出现二次释放（double free）。
 *
 *   修复后约定：nb 的所有权在 lo_xmit() 入口即移交给 ethernet_rx()，
 *   由协议栈在消费完毕后释放（ethernet_rx 的每条分支都保证 netbuf_free
 *   最终被调用）。lo_xmit() 自身不再触碰 nb 的生命周期。
 */
static int lo_xmit(struct netif *nif, struct netbuf *nb)
{
    if (!nif || !nb) return -1;
    ethernet_rx(nif, nb);
    return 0;
}

static void lo_poll(struct netif *nif) { (void)nif; }

void loopback_init(struct netns *ns)
{
    uint8_t *p = (uint8_t *)&g_lo;
    for (unsigned i = 0; i < sizeof(g_lo); ++i) p[i] = 0;

    const char *nm = "lo";
    int i = 0; while (nm[i]) { g_lo.name[i] = nm[i]; ++i; } g_lo.name[i] = '\0';
    g_lo.mac[0] = 0x00; g_lo.mac[1] = 0x00; g_lo.mac[2] = 0x00;
    g_lo.mac[3] = 0x00; g_lo.mac[4] = 0x00; g_lo.mac[5] = 0x00;
    g_lo.ip      = 0x7F000001u;   /* 127.0.0.1 */
    g_lo.mask    = 0xFF000000u;   /* 127.0.0.0/8 */
    g_lo.gateway = 0;
    g_lo.dns[0]  = 0; g_lo.dns[1] = 0;
    g_lo.up      = 1;
    g_lo.ns      = ns;
    g_lo.driver  = 0;
    g_lo.xmit    = lo_xmit;
    g_lo.poll    = lo_poll;

    if (ns) netns_add_iface(ns, &g_lo);

    /*
     * ★ 第 18A 步修复：
     *   1) 预置 ARP 缓存条目：127.0.0.1 → 00:00:00:00:00:00。
     *      否则第一次 ping 127.0.0.1 会因 arp_lookup() 未命中而进入
     *      "触发 ARP 请求并丢弃本包" 的路径，导致 ping 永远收不到响应。
     *   2) 注册 127.0.0.0/8 直连路由：使 ipv4_send() 通过 net_route_lookup()
     *      能找到 loopback 作为出接口。
     */
    uint8_t lo_mac[6] = {0, 0, 0, 0, 0, 0};
    arp_learn(&g_lo, 0x7F000001u, lo_mac);

    if (ns) {
        route_add(ns, 0x7F000000u, 0xFF000000u, 0, &g_lo, 0);
    }

    serial_printf("[NET] loopback up 127.0.0.1/8\n");
}

struct netif *loopback_iface(struct netns *ns) { (void)ns; return &g_lo; }
/*===OmniBridgeOs/kernel/arch/x64/net/loopback.c 结束===*/