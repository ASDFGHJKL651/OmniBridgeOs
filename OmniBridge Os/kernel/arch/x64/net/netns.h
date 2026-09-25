/*===OmniBridgeOs/kernel/arch/x64/net/netns.h===*/
/*
 * 网络命名空间（第 18A 步）。
 *
 * 每个命名空间独立维护接口列表、路由表、UDP/TCP 端口表与防火墙规则。
 * 内核与 OShell 使用默认命名空间；沙盒进程使用独立命名空间（默认无外部网络）。
 *
 * ★ 本次补全：
 *   - 增加 per-netns UDP 绑定表与 TCP 连接表字段。
 *   - 增加对应自旋锁与计数。
 *
 * 人工必须审查：
 *   - udp_binds / tcp_conns 为指针数组，实际对象由 udp.c / tcp.c 动态分配。
 *   - netns_destroy 当前不强制释放端口表；若后续支持 netns 销毁，必须补全。
 */
#ifndef OMNIBRIDGE_NET_NETNS_H
#define OMNIBRIDGE_NET_NETNS_H

#include <stdint.h>
#include "spinlock.h"

#define NETNS_MAX_IFACES  8
#define NETNS_MAX_ROUTES  32
#define NETNS_MAX_FWRULES 32

/* 与 udp.h / tcp.h 保持一致；避免循环包含，此处只定义上限 */
#define NETNS_MAX_UDP_BINDINGS 32
#define NETNS_MAX_TCP_CONNS    16

struct netif;
struct route_entry;
struct fw_rule;
struct udp_binding;
struct tcp_conn;

struct netns {
    uint32_t         id;
    char             name[32];

    struct netif    *ifaces[NETNS_MAX_IFACES];
    uint32_t         iface_count;

    struct route_entry *routes;
    uint32_t         route_count;

    struct fw_rule  *fw_rules;
    uint32_t         fw_count;

    /* ★ 本次补全：per-netns UDP 绑定表 */
    struct udp_binding *udp_binds[NETNS_MAX_UDP_BINDINGS];
    uint32_t         udp_count;
    spinlock_t       udp_lock;

    /* ★ 本次补全：per-netns TCP 连接表 */
    struct tcp_conn *tcp_conns[NETNS_MAX_TCP_CONNS];
    uint32_t         tcp_count;
    spinlock_t       tcp_lock;

    uint8_t          is_sandbox;    /* 1 = 沙盒命名空间：默认无外部网络 */
    uint8_t          _pad[7];

    spinlock_t       lock;
};

void netns_init(void);

struct netns *netns_default(void);
struct netns *netns_create(const char *name, uint8_t is_sandbox);
void          netns_destroy(struct netns *ns);
void          netns_add_iface(struct netns *ns, struct netif *nif);

#endif /* OMNIBRIDGE_NET_NETNS_H */
/*===OmniBridgeOs/kernel/arch/x64/net/netns.h 结束===*/