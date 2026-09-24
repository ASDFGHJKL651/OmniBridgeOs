/*===OmniBridgeOs/kernel/arch/x64/net/ipv6.h===*/
/*
 * IPv6 基础（第 18A 步）。
 *
 * 本步范围（人工必须审查）：
 *   - 解析/构造 IPv6 首部（固定 40 字节，无扩展首部）。
 *   - ICMPv6 Echo Request/Reply 最小路径。
 *   - ::1 软件回环（通过与 loopback 网卡配对实现）。
 *   - 邻居发现（NDP）：接口预留，本步返回 -1 并打印 WARN。
 *   - 不实现 SLAAC、路由通告、扩展首部、IPv6 分片。
 */
#ifndef OMNIBRIDGE_NET_IPV6_H
#define OMNIBRIDGE_NET_IPV6_H

#include <stdint.h>
#include "net.h"

/* IPv6 基础常量 */
#define IPV6_ADDR_LEN   16
#define IPV6_HDR_LEN    40

/* Next Header 值（与 IANA 一致） */
#define IPPROTO_HOPOPTS  0
#define IPPROTO_ICMPV6   58
#define IPPROTO_TCPV6    6
#define IPPROTO_UDPV6    17
#define IPPROTO_FRAGMENT 44
#define IPPROTO_NONE     59

/* ICMPv6 类型 */
#define ICMPV6_ECHO_REQUEST 128
#define ICMPV6_ECHO_REPLY   129

/* 常用 IPv6 地址（16 字节大端） */
extern const uint8_t ipv6_addr_unspecified[16];
extern const uint8_t ipv6_addr_loopback[16];
extern const uint8_t ipv6_addr_all_nodes_mc[16];

/* IPv6 固定首部（40 字节） */
struct ipv6_hdr {
    uint32_t ver_tc_fl;    /* version(4)|tc(8)|flow_label(20)，大端 */
    uint16_t payload_len;  /* 大端，不含 IPv6 首部 */
    uint8_t  next_hdr;
    uint8_t  hop_limit;
    uint8_t  src[16];
    uint8_t  dst[16];
} __attribute__((packed));

/* ICMPv6 Echo 首部（8 字节） */
struct icmpv6_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

void ipv6_init(void);

/* 接收入口：由 ethernet_rx() 在 EtherType=0x86DD 时调用，
 * 此时 nb->data 指向 IPv6 首部。 */
void ipv6_rx(struct netif *nif, struct netbuf *nb);

/* 发送一个 IPv6 包。
 *   dst       —— 目标 IPv6 地址（16 字节大端）
 *   next_hdr  —— 载荷的 Next Header 值
 *   payload   —— 载荷（不含 IPv6 首部），本函数会为其前置 40 字节首部
 *
 * 出接口选择：
 *   - 若 dst == ::1，走 loopback 接口；
 *   - 其他情况本步不支持（无外部 IPv6 地址/NDP），返回 -1。 */
int  ipv6_send(struct netif *nif, const uint8_t dst[16],
               uint8_t next_hdr, struct netbuf *payload);

/* ICMPv6 接收入口：由 ipv6_rx() 在 next_hdr=58 时调用。
 *   src / dst —— 原始包中的地址（用于构造 Reply 的伪首部） */
void icmpv6_rx(struct netif *nif, const uint8_t src[16],
               const uint8_t dst[16], struct netbuf *nb);

/* 发送 ICMPv6 Echo Request */
int  icmpv6_send_echo(struct netif *nif, const uint8_t dst[16],
                      uint16_t id, uint16_t seq,
                      const void *payload, uint32_t payload_len);

/* 简易 ping：同步等待，成功返回 0 */
int  ipv6_ping(struct netif *nif, const uint8_t dst[16],
               uint32_t timeout_ticks);

/* 校验和：IPv6 伪首部 + ICMPv6/TCP/UDP 载荷 */
uint16_t ipv6_checksum_pseudo(const uint8_t src[16], const uint8_t dst[16],
                              uint8_t next_hdr, uint32_t payload_len,
                              const void *payload);

/* 地址辅助 */
int  ipv6_addr_eq(const uint8_t a[16], const uint8_t b[16]);
int  ipv6_is_loopback(const uint8_t addr[16]);
int  ipv6_is_unspecified(const uint8_t addr[16]);
int  ipv6_is_multicast(const uint8_t addr[16]);
void ipv6_addr_copy(uint8_t dst[16], const uint8_t src[16]);
void ipv6_addr_zero(uint8_t out[16]);

/* 把主机字节序的 IPv4 地址转换为 IPv4-mapped IPv6 地址（::ffff:a.b.c.d） */
void ipv6_addr_from_ipv4(uint8_t out[16], uint32_t ipv4_host_order);

/* 邻居发现接口预留（本步为桩；返回 -1 并打印 WARN）。
 * 后续步骤（完整 IPv6）应填充 NDP 缓存并返回目标 MAC。 */
int  ipv6_ndp_resolve(struct netif *nif, const uint8_t dst[16],
                      uint8_t out_mac[6]);

#endif /* OMNIBRIDGE_NET_IPV6_H */
/*===OmniBridgeOs/kernel/arch/x64/net/ipv6.h 结束===*/