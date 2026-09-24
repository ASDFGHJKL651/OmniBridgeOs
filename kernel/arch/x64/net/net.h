/*===OmniBridgeOs/kernel/arch/x64/net/net.h===*/
/*
 * 网络栈顶层接口（第 18A 步）。
 *
 * 关键约束（人工必须审查）：
 *   - 所有用户态可达的路径必须经过 check_permission()。
 *   - 沙盒进程（sandbox_flags != 0）的网络操作直接返回 OB_ENETUNREACH。
 *   - 所有对外接口在进入协议栈前检查命名空间归属。
 */
#ifndef OMNIBRIDGE_NET_NET_H
#define OMNIBRIDGE_NET_NET_H

#include <stdint.h>
#include "task.h"
#include "netbuf.h"
#include "netns.h"

#ifndef OB_ENETUNREACH
#define OB_ENETUNREACH (-101)
#endif

/* 常用端口 */
#define NET_PORT_DHCP_SERVER 67
#define NET_PORT_DHCP_CLIENT 68
#define NET_PORT_DNS         53

/* 字节序辅助 */
static inline uint16_t net_htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}
static inline uint16_t net_ntohs(uint16_t v) { return net_htons(v); }

static inline uint32_t net_htonl(uint32_t v)
{
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}
static inline uint32_t net_ntohl(uint32_t v) { return net_htonl(v); }

/* 网络接口 */
struct netif {
    char     name[16];
    uint8_t  mac[6];
    uint8_t  _pad[2];
    uint32_t ip;         /* 主机字节序 */
    uint32_t mask;       /* 主机字节序 */
    uint32_t gateway;    /* 主机字节序 */
    uint32_t dns[2];     /* 主机字节序 */
    uint8_t  up;
    uint8_t  _pad2[3];
    struct netns *ns;
    void    *driver;     /* 驱动私有 */
    int (*xmit)(struct netif *nif, struct netbuf *nb);
    void (*poll)(struct netif *nif);
};

/* 初始化顶层网络栈 */
void net_init(void);

/* 主入口：由驱动（virtio_net、loopback）调用，向协议栈上送一帧 */
void net_rx(struct netif *nif, struct netbuf *nb);

/* 向指定 netif 发送帧（由 IP 层调用） */
int  net_tx(struct netif *nif, struct netbuf *nb);

/* 按 IP 查找出接口（最长前缀匹配，由 route.c 提供） */
struct netif *net_route_lookup(struct netns *ns, uint32_t dst_ip);

/* 当前任务网络权限检查：沙盒拒绝；无 netns 拒绝 */
int  net_check_task_access(struct task_t *cur);

/* 打印已配置接口 */
void net_dump_ifaces(void);

/* ★ 第 18A 步补充：定时器轮询入口
 *
 * 语义：由 sched_tick() 或空闲线程周期调用，用于：
 *   - 轮询所有非中断驱动的网络接口（VirtIO）
 *   - 驱动 TCP 重传/超时定时器
 * 该函数在中断上下文与进程上下文均可安全调用（内部各协议层自加锁）。
 */
void net_tick(void);

/* 是否存在真实以太网接口（用于 OShell / 自检分支） */
int  net_has_eth(void);

#endif /* OMNIBRIDGE_NET_NET_H */
/*===OmniBridgeOs/kernel/arch/x64/net/net.h 结束===*/