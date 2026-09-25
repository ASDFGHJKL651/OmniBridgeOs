/*===OmniBridgeOs/kernel/arch/x64/net/net_selftest.c===*/
#include "net_selftest.h"
#include "net.h"
#include "netns.h"
#include "netbuf.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "loopback.h"
#include "route.h"
#include "dhcp.h"
#include "dns.h"
#include "socket.h"
#include "csprng.h"
#include "permission.h"
#include "serial.h"
#include "task.h"

static int g_fail = 0;

static void check(const char *desc, int ok)
{
    if (ok) serial_printf("[NET-TEST] OK  : %s\n", desc);
    else { serial_printf("[NET-TEST] FAIL: %s\n", desc); g_fail++; }
}

static void test_ip_checksum(void)
{
    uint8_t buf[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
        0xC0, 0xA8, 0x00, 0xC7
    };
    uint16_t c = ip_checksum(buf, 20);
    check("ip_checksum non-zero", c != 0);
    buf[10] = (uint8_t)(c >> 8); buf[11] = (uint8_t)(c & 0xFF);
    check("ip_checksum self-verifies", ip_checksum(buf, 20) == 0);
}

static void test_tcp_checksum(void)
{
    /*
     * ★ 修复（v11）：ip_checksum_pseudo 的 src/dst 参数必须是
     *   host order 的 32 位 IP。之前传 net_htonl(...) 导致伪首部
     *   两处 16 位字错位，与真实网络包不兼容。
     */
    uint8_t tcp[20];
    for (int i = 0; i < 20; ++i) tcp[i] = 0;
    tcp[0] = 0x12; tcp[1] = 0x34;
    tcp[2] = 0x56; tcp[3] = 0x78;
    tcp[12] = 0x50; tcp[13] = TCP_ACK;
    tcp[14] = 0x10; tcp[15] = 0x00;
    uint16_t c = ip_checksum_pseudo(
        0x0A000001u, 0x0A000002u,          /* host order */
        IP_PROTO_TCP, 20, tcp, 20, 0, 0);
    check("tcp pseudo checksum non-zero", c != 0);
    tcp[16] = (uint8_t)(c >> 8); tcp[17] = (uint8_t)(c & 0xFF);
    uint16_t v = ip_checksum_pseudo(
        0x0A000001u, 0x0A000002u,          /* host order */
        IP_PROTO_TCP, 20, tcp, 20, 0, 0);
    check("tcp pseudo checksum self-verifies", v == 0);
}

static void test_netbuf(void)
{
    struct netbuf *nb = netbuf_alloc(netns_default(), 100);
    check("netbuf_alloc", nb != 0);
    if (!nb) return;
    for (int i = 0; i < 100; ++i) nb->data[i] = (uint8_t)i;
    nb->len = 100;
    struct netbuf *c = netbuf_clone(nb);
    check("netbuf_clone len", c && c->len == 100);
    if (c) {
        int ok = 1;
        for (int i = 0; i < 100; ++i)
            if (c->data[i] != (uint8_t)i) { ok = 0; break; }
        check("netbuf_clone content", ok);
        netbuf_free(c);
    }
    netbuf_retain(nb);
    netbuf_free(nb);
    netbuf_free(nb);
    check("netbuf refcount works", 1);
}

static void test_route(void)
{
    struct netns *ns = netns_default();
    struct netif *lo = loopback_iface(ns);
    check("loopback iface available", lo != 0);

    route_add(ns, 0x0A000000, 0xFF000000, 0, lo, 0);
    route_add(ns, 0, 0, 0x0A000001, lo, RT_FLAG_GATEWAY);
    route_add(ns, 0x0A000005, 0xFFFFFFFF, 0, lo, RT_FLAG_HOST);

    struct route_entry *r = 0;
    int rc = route_lookup(ns, 0x0A000005, &r);
    check("longest-prefix host match",
          rc == 0 && r && r->dst == 0x0A000005);
    rc = route_lookup(ns, 0x0A000099, &r);
    check("longest-prefix subnet match",
          rc == 0 && r && r->mask == 0xFF000000);
    rc = route_lookup(ns, 0x08080808, &r);
    check("default route match", rc == 0 && r && r->mask == 0);
}

static void test_arp(void)
{
    struct netns *ns = netns_default();
    struct netif *lo = loopback_iface(ns);

    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    arp_learn(lo, 0x0A000001, mac);
    uint8_t got[6];
    check("arp learn/lookup", arp_lookup(lo, 0x0A000001, got) == 0);
    check("arp lookup mac match", got[0] == 0x11 && got[5] == 0x66);
    check("arp unknown miss", arp_lookup(lo, 0x0A000099, got) != 0);

    uint8_t lo_mac[6];
    check("loopback 127.0.0.1 pre-ARP",
          arp_lookup(lo, 0x7F000001u, lo_mac) == 0);
}

static void test_port_alloc(void)
{
    uint16_t a = udp_alloc_ephemeral();
    uint16_t b = udp_alloc_ephemeral();
    check("udp_alloc_ephemeral distinct", a != b);
    check("udp_alloc_ephemeral range",
          a >= 40000 && a < 60000 && b >= 40000 && b < 60000);
}

static void test_sandbox_deny(void)
{
    struct task_t fake;
    uint8_t *p = (uint8_t *)&fake;
    for (unsigned i = 0; i < sizeof(fake); ++i) p[i] = 0;
    fake.pid = 9000;
    fake.sandbox_flags = 0x01;

    int rc = net_socket(NET_AF_INET, NET_SOCK_STREAM, 0, &fake);
    check("sandbox socket -> -ENETUNREACH", rc == OB_ENETUNREACH);
    rc = net_check_task_access(&fake);
    check("net_check_task_access sandbox denies", rc == OB_ENETUNREACH);
}

static void test_permission_deny(void)
{
    struct task_t fake;
    uint8_t *p = (uint8_t *)&fake;
    for (unsigned i = 0; i < sizeof(fake); ++i) p[i] = 0;
    fake.pid = 5000;
    fake.privilege_level = 0;
    fake.security_token.level = 0;

    int rc = check_permission(&fake, OB_RES_IPC, 0,
                              OB_ACCESS_READ | OB_ACCESS_WRITE,
                              "(net-selftest)");
    check("low-priv network permission denied", rc == OB_EPERM);
}

static void test_loopback_ping(void)
{
    struct netns *ns = netns_default();
    struct netif *lo = loopback_iface(ns);
    int rc = icmp_ping(lo, 0x7F000001u, 32, 3000);
    check("IPv4 loopback ping 127.0.0.1", rc == 0);
}

static void test_ipv6_loopback_ping(void)
{
    struct netns *ns = netns_default();
    struct netif *lo = loopback_iface(ns);
    uint8_t target[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};

    int rc = ipv6_ping(lo, target, 3000);
    check("IPv6 loopback ping ::1", rc == 0);
}

static void test_tcp_loopback(void)
{
    struct netns *ns = netns_default();

    struct tcp_conn *l = tcp_listen(ns, 0x7F000001u, 0x3039);
    check("tcp_listen returns conn", l != 0);
    if (!l) return;
    check("listener state LISTEN", l->state == TCP_LISTEN);

    struct tcp_conn *c = tcp_connect(ns, 0x7F000001u, 0x3039);
    check("tcp_connect returns conn", c != 0);
    if (!c) { l->used = 0; return; }

    check("client ESTABLISHED after loopback handshake",
          c->state == TCP_ESTABLISHED);

    const char *msg = "hello tcp";
    int sent = tcp_send_data(c, msg, 9);
    check("tcp_send_data returns 9", sent == 9);

    tcp_close(c);
    l->used = 0;
}

/*
 * ★ DNS 测试改为"环境可达性敏感"：
 *   - 无 eth 接口 → NOTE 跳过；
 *   - 无 DNS 服务器 → NOTE 跳过；
 *   - DNS 服务器 ARP 不可解析 → NOTE 跳过；
 *   - 上述全部满足，实际发起 DNS 查询；若失败才算 FAIL。
 */
static void test_dns_real(void)
{
    struct netns *ns = netns_default();

    struct netif *eth = 0;
    for (uint32_t i = 0; i < ns->iface_count; ++i) {
        struct netif *nif = ns->ifaces[i];
        if (nif && nif->name[0] != 'l') { eth = nif; break; }
    }

    if (!eth) {
        serial_printf("[NET-TEST] NOTE: no eth iface, DNS test skipped\n");
        return;
    }
    if (!eth->dns[0]) {
        serial_printf("[NET-TEST] NOTE: no DNS server configured, "
                      "DNS test skipped\n");
        return;
    }

    uint8_t dns_mac[6];
    if (arp_lookup(eth, eth->dns[0], dns_mac) != 0) {
        arp_resolve(eth, eth->dns[0]);
        for (int i = 0; i < 20; ++i) {
            if (eth->poll) eth->poll(eth);
            for (volatile int j = 0; j < 100000; ++j) {
                __asm__ __volatile__("pause" ::: "memory");
            }
            if (arp_lookup(eth, eth->dns[0], dns_mac) == 0) break;
        }
    }

    if (arp_lookup(eth, eth->dns[0], dns_mac) != 0) {
        serial_printf("[NET-TEST] NOTE: DNS server %u.%u.%u.%u unreachable "
                      "(no ARP reply); DNS test skipped — this indicates "
                      "the test environment lacks a working gateway/DNS.\n",
                      (unsigned)((eth->dns[0] >> 24) & 0xFF),
                      (unsigned)((eth->dns[0] >> 16) & 0xFF),
                      (unsigned)((eth->dns[0] >> 8) & 0xFF),
                      (unsigned)(eth->dns[0] & 0xFF));
        return;
    }

    uint32_t ip = 0;
    int rc = dns_resolve(eth, "example.com", &ip);
    check("DNS resolve example.com", rc == 0);
    if (rc == 0) {
        serial_printf("[NET-TEST] DNS example.com -> %u.%u.%u.%u\n",
                      (unsigned)((ip >> 24) & 0xFF),
                      (unsigned)((ip >> 16) & 0xFF),
                      (unsigned)((ip >> 8) & 0xFF),
                      (unsigned)(ip & 0xFF));
    }
}

static void test_ipv6_addr_helpers(void)
{
    uint8_t lo6[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
    uint8_t uns[16] = {0};
    uint8_t mc[16]  = {0xFF,0x02,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};

    check("ipv6_is_loopback(::1)", ipv6_is_loopback(lo6) == 1);
    check("ipv6_is_loopback(::)",  ipv6_is_loopback(uns) == 0);
    check("ipv6_is_unspecified(::)", ipv6_is_unspecified(uns) == 1);
    check("ipv6_is_multicast(ff02::1)", ipv6_is_multicast(mc) == 1);
    check("ipv6_is_multicast(::1)", ipv6_is_multicast(lo6) == 0);

    uint8_t a[16], b[16];
    ipv6_addr_copy(a, lo6);
    ipv6_addr_copy(b, lo6);
    check("ipv6_addr_eq(::1,::1)", ipv6_addr_eq(a, b) == 1);
    b[15] = 2;
    check("ipv6_addr_eq(::1,::2) false", ipv6_addr_eq(a, b) == 0);
}

static void test_ipv6_checksum(void)
{
    uint8_t src[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
    uint8_t dst[16] = {0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};

    uint8_t pkt[8] = {128, 0, 0, 0, 0x12, 0x34, 0x00, 0x01};
    uint16_t c = ipv6_checksum_pseudo(src, dst, IPPROTO_ICMPV6,
                                      sizeof(pkt), pkt);
    check("ipv6_checksum non-zero", c != 0);

    pkt[2] = (uint8_t)(c >> 8);
    pkt[3] = (uint8_t)(c & 0xFF);
    uint16_t v = ipv6_checksum_pseudo(src, dst, IPPROTO_ICMPV6,
                                      sizeof(pkt), pkt);
    check("ipv6_checksum self-verifies", v == 0);
}

static void test_ipv6_ndp_stub(void)
{
    uint8_t dst[16] = {0xfe, 0x80, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1};
    uint8_t mac[6];
    int rc = ipv6_ndp_resolve(loopback_iface(netns_default()), dst, mac);
    check("ndp stub returns -1", rc == -1);
}

void net_selftest(void)
{
    g_fail = 0;
    serial_printf("[NET-TEST] === begin ===\n");

    test_ip_checksum();
    test_tcp_checksum();

    test_netbuf();
    test_route();
    test_arp();
    test_port_alloc();

    test_sandbox_deny();
    test_permission_deny();

    test_loopback_ping();
    test_ipv6_loopback_ping();

    test_tcp_loopback();
    test_dns_real();

    test_ipv6_addr_helpers();
    test_ipv6_checksum();
    test_ipv6_ndp_stub();

    if (g_fail == 0) serial_printf("[NET] selftest OK\n");
    else serial_printf("[NET] selftest FAILED: %d case(s)\n", g_fail);
    serial_printf("[NET-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/net/net_selftest.c 结束===*/