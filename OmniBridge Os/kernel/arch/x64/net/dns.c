/*===OmniBridgeOs/kernel/arch/x64/net/dns.c===*/
#include "dns.h"
#include "udp.h"
#include "arp.h"           /* ★ 新增：DNS 测试前先检查 ARP 缓存 */
#include "csprng.h"
#include "serial.h"
#include "spinlock.h"

static struct dns_cache_entry g_cache[DNS_CACHE_MAX];
static spinlock_t g_lock = SPINLOCK_INIT;
static uint64_t   g_ticks = 0;

static uint16_t g_pending_id = 0;
static uint32_t g_pending_ip = 0;
static uint8_t  g_pending_ip6[16];
static int      g_pending = 0;
static int      g_pending_is_aaaa = 0;

void dns_init(void)
{
    spin_lock_init(&g_lock);
    for (int i = 0; i < DNS_CACHE_MAX; ++i) {
        g_cache[i].valid = 0;
        g_cache[i].ip = 0;
        g_cache[i].expire_tick = 0;
    }
    serial_printf("[DNS] init: cache=%d\n", DNS_CACHE_MAX);
}

static int name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == '\0' && *b == '\0';
}

static void dns_cache_put(const char *name, uint32_t ip, uint32_t ttl_ticks)
{
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    int slot = -1;
    for (int i = 0; i < DNS_CACHE_MAX; ++i) {
        if (g_cache[i].valid && name_eq(g_cache[i].name, name)) {
            slot = i; break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < DNS_CACHE_MAX; ++i) {
            if (!g_cache[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) slot = 0;
    int i = 0;
    while (name[i] && i < 63) { g_cache[slot].name[i] = name[i]; ++i; }
    g_cache[slot].name[i] = '\0';
    g_cache[slot].ip = ip;
    g_cache[slot].expire_tick = g_ticks + ttl_ticks;
    g_cache[slot].valid = 1;
    spin_unlock_irqrestore(&g_lock, fl);
}

static int dns_cache_get(const char *name, uint32_t *out_ip)
{
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    int found = 0;
    for (int i = 0; i < DNS_CACHE_MAX; ++i) {
        if (g_cache[i].valid && name_eq(g_cache[i].name, name) &&
            g_ticks < g_cache[i].expire_tick) {
            *out_ip = g_cache[i].ip;
            found = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&g_lock, fl);
    return found ? 0 : -1;
}

int dns_handle_response(const uint8_t *pkt, uint32_t len, uint32_t *out_ip)
{
    if (!pkt || len < 12) return -1;
    uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    if (id != g_pending_id) return -1;
    uint16_t flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if ((flags & 0x8000) == 0) return -1;
    if ((flags & 0x000F) != 0) return -1;

    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint16_t an = (uint16_t)((pkt[6] << 8) | pkt[7]);
    if (an == 0) return -1;

    uint32_t off = 12;
    for (int q = 0; q < qd; ++q) {
        while (off < len && pkt[off] != 0) off++;
        off += 5;
    }

    for (int a = 0; a < an && off + 12 <= len; ++a) {
        if ((pkt[off] & 0xC0) == 0xC0) off += 2;
        else { while (off < len && pkt[off]) off++; off++; }
        if (off + 10 > len) break;
        uint16_t type = (uint16_t)((pkt[off] << 8) | pkt[off+1]);
        uint16_t rdlen = (uint16_t)((pkt[off+8] << 8) | pkt[off+9]);
        off += 10;
        if (off + rdlen > len) break;
        if (type == 1 && rdlen == 4) {
            uint32_t ip = ((uint32_t)pkt[off] << 24) |
                          ((uint32_t)pkt[off+1] << 16) |
                          ((uint32_t)pkt[off+2] << 8) |
                          pkt[off+3];
            if (out_ip) *out_ip = ip;
            g_pending_ip = ip;
            g_pending = 1;
            g_pending_is_aaaa = 0;
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

int dns_handle_response_aaaa(const uint8_t *pkt, uint32_t len,
                             uint8_t out_ip6[16])
{
    if (!pkt || len < 12) return -1;
    uint16_t id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    if (id != g_pending_id) return -1;
    uint16_t flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if ((flags & 0x8000) == 0) return -1;
    if ((flags & 0x000F) != 0) return -1;

    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);
    uint16_t an = (uint16_t)((pkt[6] << 8) | pkt[7]);
    if (an == 0) return -1;

    uint32_t off = 12;
    for (int q = 0; q < qd; ++q) {
        while (off < len && pkt[off] != 0) off++;
        off += 5;
    }

    for (int a = 0; a < an && off + 12 <= len; ++a) {
        if ((pkt[off] & 0xC0) == 0xC0) off += 2;
        else { while (off < len && pkt[off]) off++; off++; }
        if (off + 10 > len) break;
        uint16_t type = (uint16_t)((pkt[off] << 8) | pkt[off+1]);
        uint16_t rdlen = (uint16_t)((pkt[off+8] << 8) | pkt[off+9]);
        off += 10;
        if (off + rdlen > len) break;
        if (type == 28 && rdlen == 16) {
            for (int i = 0; i < 16; ++i) {
                out_ip6[i] = pkt[off + i];
                g_pending_ip6[i] = pkt[off + i];
            }
            g_pending = 1;
            g_pending_is_aaaa = 1;
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

static void dns_udp_cb(struct netif *nif, uint32_t src_ip,
                       uint16_t src_port, uint16_t dst_port,
                       const void *data, uint32_t len, void *arg)
{
    (void)nif; (void)src_ip; (void)src_port; (void)dst_port; (void)arg;

    uint32_t ip = 0;
    if (dns_handle_response((const uint8_t *)data, len, &ip) == 0) {
        g_pending_ip = ip;
        g_pending = 1;
        return;
    }

    uint8_t ip6[16];
    if (dns_handle_response_aaaa((const uint8_t *)data, len, ip6) == 0) {
        g_pending = 1;
        return;
    }
}

static int dns_build_query(uint16_t id, const char *name, uint16_t qtype,
                           uint8_t *pkt, uint32_t pkt_cap)
{
    uint32_t p = 0;
    if (pkt_cap < 16) return -1;

    pkt[p++] = (uint8_t)(id >> 8);
    pkt[p++] = (uint8_t)(id & 0xFF);
    pkt[p++] = 0x01; pkt[p++] = 0x00;
    pkt[p++] = 0; pkt[p++] = 1;
    pkt[p++] = 0; pkt[p++] = 0;
    pkt[p++] = 0; pkt[p++] = 0;
    pkt[p++] = 0; pkt[p++] = 0;

    const char *s = name;
    while (*s) {
        const char *d = s;
        while (*d && *d != '.') d++;
        uint8_t l = (uint8_t)(d - s);
        if (l == 0 || l > 63) return -1;
        if (p + 1 + l >= pkt_cap) return -1;
        pkt[p++] = l;
        for (uint8_t i = 0; i < l; ++i) pkt[p++] = (uint8_t)s[i];
        if (*d == '.') s = d + 1; else break;
    }
    if (p + 1 >= pkt_cap) return -1;
    pkt[p++] = 0;
    if (p + 4 > pkt_cap) return -1;
    pkt[p++] = (uint8_t)(qtype >> 8);
    pkt[p++] = (uint8_t)(qtype & 0xFF);
    pkt[p++] = 0; pkt[p++] = 1;
    return (int)p;
}

/*
 * ★ 本轮关键修复：DNS 等待窗口过短。
 *
 *   上一版本 DNS_RESEND_INTERVAL=40，每次迭代只放一个 pause 指令
 *   （几十纳秒），一轮总等待 < 几微秒。QEMU SLIRP 响应 DNS 需要
 *   几毫秒到几十毫秒，导致必然超时。
 *
 *   本轮参数：
 *     DNS_MAX_ROUNDS  = 3     每轮总等待约 200ms，共约 600ms
 *     DNS_POLL_ITERS  = 20    每轮 20 次 poll
 *     DNS_PAUSE_EACH  = 500000 每次 poll 之间约 10ms pause
 *
 *   这样每轮 20 × 10ms = 200ms，3 轮 = 600ms。既保证 SLIRP 有时间
 *   响应，又不会让启动流程卡顿数秒。
 */
#define DNS_MAX_ROUNDS  3
#define DNS_POLL_ITERS  20
#define DNS_PAUSE_EACH  500000

int dns_resolve(struct netif *nif, const char *name, uint32_t *out_ip)
{
    if (!nif || !name || !out_ip) return -1;

    g_ticks++;

    if (dns_cache_get(name, out_ip) == 0) {
        serial_printf("[DNS] cache hit %s -> %u.%u.%u.%u\n", name,
                      (unsigned)((*out_ip >> 24) & 0xFF),
                      (unsigned)((*out_ip >> 16) & 0xFF),
                      (unsigned)((*out_ip >> 8) & 0xFF),
                      (unsigned)(*out_ip & 0xFF));
        return 0;
    }

    if (!nif->dns[0]) {
        serial_printf("[DNS] no DNS server configured\n");
        return -1;
    }

    /*
     * ★ 前置检查：DNS 服务器的 ARP 必须可解析。
     *   若否则直接放弃，不再徒劳发送 DNS 查询——这可以节省时间，
     *   也让"网络不可用"的诊断更清晰。dns_resolve 本身仍然返回 -1，
     *   由调用方决定如何处理。
     */
    {
        uint8_t dns_mac[6];
        if (arp_lookup(nif, nif->dns[0], dns_mac) != 0) {
            arp_resolve(nif, nif->dns[0]);
            /* 给 ARP 一个短暂的求解窗口（约 100ms） */
            for (int i = 0; i < 10; ++i) {
                if (nif->poll) nif->poll(nif);
                for (volatile int j = 0; j < 100000; ++j) {
                    __asm__ __volatile__("pause" ::: "memory");
                }
                if (arp_lookup(nif, nif->dns[0], dns_mac) == 0) break;
            }
            if (arp_lookup(nif, nif->dns[0], dns_mac) != 0) {
                serial_printf("[DNS] DNS server %u.%u.%u.%u ARP unresolved, "
                              "abort query\n",
                              (unsigned)((nif->dns[0] >> 24) & 0xFF),
                              (unsigned)((nif->dns[0] >> 16) & 0xFF),
                              (unsigned)((nif->dns[0] >> 8) & 0xFF),
                              (unsigned)(nif->dns[0] & 0xFF));
                return -2;   /* -2 表示"环境不可达"，与超时区分 */
            }
        }
    }

    uint16_t id = csprng_u16();
    g_pending_id = id;
    g_pending = 0;
    g_pending_is_aaaa = 0;

    uint8_t pkt[256];
    int p = dns_build_query(id, name, 1 /* A */, pkt, sizeof(pkt));
    if (p < 0) return -1;

    uint16_t sport = udp_alloc_ephemeral();
    udp_bind(nif->ns, sport, 0, dns_udp_cb, 0);

    for (uint32_t round = 0; round < DNS_MAX_ROUNDS; ++round) {
        udp_send(nif, nif->dns[0], sport, 53, pkt, (uint32_t)p);

        for (uint32_t i = 0; i < DNS_POLL_ITERS; ++i) {
            if (g_pending && !g_pending_is_aaaa) break;
            if (nif->poll) nif->poll(nif);
            for (volatile int j = 0; j < DNS_PAUSE_EACH; ++j) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }

        if (g_pending && !g_pending_is_aaaa) {
            *out_ip = g_pending_ip;
            dns_cache_put(name, g_pending_ip, 6000);
            udp_unbind(nif->ns, sport);
            serial_printf("[DNS] resolved %s -> %u.%u.%u.%u\n", name,
                          (unsigned)((*out_ip >> 24) & 0xFF),
                          (unsigned)((*out_ip >> 16) & 0xFF),
                          (unsigned)((*out_ip >> 8) & 0xFF),
                          (unsigned)(*out_ip & 0xFF));
            return 0;
        }
    }

    udp_unbind(nif->ns, sport);
    serial_printf("[DNS] timeout for %s\n", name);
    return -1;
}

int dns_resolve_aaaa(struct netif *nif, const char *name, uint8_t out_ip6[16])
{
    if (!nif || !name || !out_ip6) return -1;
    if (!nif->dns[0]) {
        serial_printf("[DNS] no DNS server configured\n");
        return -1;
    }

    /* AAAA 同样先做 ARP 前置检查 */
    {
        uint8_t dns_mac[6];
        if (arp_lookup(nif, nif->dns[0], dns_mac) != 0) {
            arp_resolve(nif, nif->dns[0]);
            for (int i = 0; i < 10; ++i) {
                if (nif->poll) nif->poll(nif);
                for (volatile int j = 0; j < 100000; ++j) {
                    __asm__ __volatile__("pause" ::: "memory");
                }
                if (arp_lookup(nif, nif->dns[0], dns_mac) == 0) break;
            }
            if (arp_lookup(nif, nif->dns[0], dns_mac) != 0) {
                serial_printf("[DNS] DNS server ARP unresolved, abort AAAA\n");
                return -2;
            }
        }
    }

    uint16_t id = csprng_u16();
    g_pending_id = id;
    g_pending = 0;
    g_pending_is_aaaa = 0;

    uint8_t pkt[256];
    int p = dns_build_query(id, name, 28 /* AAAA */, pkt, sizeof(pkt));
    if (p < 0) return -1;

    uint16_t sport = udp_alloc_ephemeral();
    udp_bind(nif->ns, sport, 0, dns_udp_cb, 0);

    for (uint32_t round = 0; round < DNS_MAX_ROUNDS; ++round) {
        udp_send(nif, nif->dns[0], sport, 53, pkt, (uint32_t)p);

        for (uint32_t i = 0; i < DNS_POLL_ITERS; ++i) {
            if (g_pending && g_pending_is_aaaa) break;
            if (nif->poll) nif->poll(nif);
            for (volatile int j = 0; j < DNS_PAUSE_EACH; ++j) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }

        if (g_pending && g_pending_is_aaaa) {
            for (int k = 0; k < 16; ++k) out_ip6[k] = g_pending_ip6[k];
            udp_unbind(nif->ns, sport);
            serial_printf("[DNS] resolved AAAA %s\n", name);
            return 0;
        }
    }

    udp_unbind(nif->ns, sport);
    serial_printf("[DNS] AAAA timeout for %s\n", name);
    return -1;
}

void dns_dump_cache(void)
{
    serial_printf("[DNS] cache dump:\n");
    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    for (int i = 0; i < DNS_CACHE_MAX; ++i) {
        if (!g_cache[i].valid) continue;
        serial_printf("  %s -> %u.%u.%u.%u (expire=%llu)\n",
                      g_cache[i].name,
                      (unsigned)((g_cache[i].ip >> 24) & 0xFF),
                      (unsigned)((g_cache[i].ip >> 16) & 0xFF),
                      (unsigned)((g_cache[i].ip >> 8) & 0xFF),
                      (unsigned)(g_cache[i].ip & 0xFF),
                      (unsigned long long)g_cache[i].expire_tick);
    }
    spin_unlock_irqrestore(&g_lock, fl);
}
/*===OmniBridgeOs/kernel/arch/x64/net/dns.c 结束===*/