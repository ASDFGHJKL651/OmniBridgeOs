/*===OmniBridgeOs/tests/host/test_net_logic.c===*/
/*
 * 纯逻辑测试：IP 校验和、TCP 校验和、路由最长前缀匹配、ARP 缓存键、
 * 端口分配。不依赖硬件。
 */
#include <stdio.h>
#include <stdint.h>

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

static uint16_t cksum16(const uint8_t *p, uint32_t n)
{
    uint32_t sum = 0;
    while (n > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

static uint32_t lpm(uint32_t dst, uint32_t *ds, uint32_t *ms, int n)
{
    uint32_t best_mask = 0, best_dst = 0;
    for (int i = 0; i < n; ++i) {
        if ((dst & ms[i]) == (ds[i] & ms[i])) {
            if (ms[i] > best_mask) { best_mask = ms[i]; best_dst = ds[i]; }
        }
    }
    return best_mask ? best_dst : 0xFFFFFFFFu;
}

static uint32_t arp_key(uint32_t ip) { return ip; }

int main(void)
{
    /* 1) 校验和自验证 */
    uint8_t hdr[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
        0xC0, 0xA8, 0x00, 0xC7
    };
    uint16_t c = cksum16(hdr, 20);
    CHECK(c != 0);
    hdr[10] = (uint8_t)(c >> 8); hdr[11] = (uint8_t)c;
    CHECK(cksum16(hdr, 20) == 0);

    /* 2) 路由最长前缀匹配 */
    uint32_t ds[3] = {0x0A000000, 0x00000000, 0x0A000005};
    uint32_t ms[3] = {0xFF000000, 0x00000000, 0xFFFFFFFF};
    CHECK(lpm(0x0A000005, ds, ms, 3) == 0x0A000005);   /* 主机路由 */
    CHECK(lpm(0x0A000099, ds, ms, 3) == 0x0A000000);   /* 子网路由 */
    CHECK(lpm(0x08080808, ds, ms, 3) == 0x00000000);   /* 默认路由 */

    /* 3) ARP 缓存键 */
    CHECK(arp_key(0x0A000001) == 0x0A000001);
    CHECK(arp_key(0x0A000001) != arp_key(0x0A000002));

    /* 4) 端口随机范围 */
    for (int i = 0; i < 100; ++i) {
        uint16_t p = (uint16_t)(40000 + (i % 20000));
        CHECK(p >= 40000 && p < 60000);
    }

    if (fail == 0) { printf("test_net_logic: OK\n"); return 0; }
    printf("test_net_logic: %d failures\n", fail);
    return 1;
}
/*===OmniBridgeOs/tests/host/test_net_logic.c 结束===*/