/*===OmniBridgeOs/kernel/arch/x64/net/socket.c===*/
#include "socket.h"
#include "tcp.h"
#include "udp.h"
#include "permission.h"
#include "audit.h"
#include "serial.h"
#include "spinlock.h"
#include "../vfs.h"

/* ★ 修复 3：网络数据到达后唤醒 poll 等待者 */
extern void select_notify_wakeup(void);

static struct net_socket g_socks[NET_FD_MAX];
static spinlock_t        g_lock = SPINLOCK_INIT;

void net_socket_init(void)
{
    spin_lock_init(&g_lock);
    for (int i = 0; i < NET_FD_MAX; ++i) g_socks[i].fd = -1;
    serial_printf("[SOCKET] init: max_fd=%d\n", NET_FD_MAX);
}

static int net_check_task(struct task_t *cur)
{
    if (!cur) return OB_EPERM;
    if (cur->sandbox_flags != 0) return OB_ENETUNREACH;
    return 0;
}

int net_socket(int domain, int type, int protocol, struct task_t *cur)
{
    (void)protocol;
    int rc = net_check_task(cur);
    if (rc != 0) return rc;

    uint64_t fl;
    spin_lock_irqsave(&g_lock, &fl);
    int fd = -1;
    for (int i = 0; i < NET_FD_MAX; ++i) {
        if (g_socks[i].fd < 0) { fd = i; break; }
    }
    if (fd < 0) { spin_unlock_irqrestore(&g_lock, fl); return OB_EAGAIN; }
    uint8_t *p = (uint8_t *)&g_socks[fd];
    for (unsigned i = 0; i < sizeof(g_socks[fd]); ++i) p[i] = 0;
    g_socks[fd].fd     = fd;
    g_socks[fd].domain = domain;
    g_socks[fd].type   = type;
    g_socks[fd].ns     = netns_default();
    spin_unlock_irqrestore(&g_lock, fl);
    return fd;
}

int net_bind(int fd, uint32_t ip, uint16_t port, struct task_t *cur)
{
    int rc = net_check_task(cur);
    if (rc != 0) return rc;
    if (fd < 0 || fd >= NET_FD_MAX || g_socks[fd].fd < 0) return OB_EBADF;

    g_socks[fd].local_ip   = ip;
    g_socks[fd].local_port = port;
    g_socks[fd].bound      = 1;

    if (g_socks[fd].type == NET_SOCK_DGRAM) {
        return udp_bind(g_socks[fd].ns, port, ip, 0, 0);
    }
    return 0;
}

int net_listen(int fd, int backlog, struct task_t *cur)
{
    (void)backlog;
    int rc = net_check_task(cur);
    if (rc != 0) return rc;
    if (fd < 0 || fd >= NET_FD_MAX || g_socks[fd].fd < 0) return OB_EBADF;
    if (g_socks[fd].type != NET_SOCK_STREAM) return OB_EINVAL;

    struct tcp_conn *c = tcp_listen(g_socks[fd].ns,
                                    g_socks[fd].local_ip,
                                    g_socks[fd].local_port);
    if (!c) return OB_ENOMEM;
    g_socks[fd].tcp       = c;
    g_socks[fd].listening = 1;
    return 0;
}

int net_accept(int fd, struct task_t *cur)
{
    int rc = net_check_task(cur);
    if (rc != 0) return rc;
    if (fd < 0 || fd >= NET_FD_MAX || g_socks[fd].fd < 0) return OB_EBADF;
    return OB_ENOSYS;
}

int net_connect(int fd, uint32_t ip, uint16_t port, struct task_t *cur)
{
    int rc = net_check_task(cur);
    if (rc != 0) return rc;
    if (fd < 0 || fd >= NET_FD_MAX || g_socks[fd].fd < 0) return OB_EBADF;

    if (g_socks[fd].type == NET_SOCK_STREAM) {
        struct tcp_conn *c = tcp_connect(g_socks[fd].ns, ip, port);
        if (!c) return OB_ENOMEM;
        g_socks[fd].tcp       = c;
        g_socks[fd].remote_ip = ip;
        g_socks[fd].remote_port = port;
        g_socks[fd].connected = 1;
        return 0;
    }
    g_socks[fd].remote_ip   = ip;
    g_socks[fd].remote_port = port;
    g_socks[fd].connected   = 1;
    return 0;
}

int net_send(int fd, const void *buf, uint32_t len, struct task_t *cur)
{
    int rc = net_check_task(cur);
    if (rc != 0) return rc;
    if (fd < 0 || fd >= NET_FD_MAX || g_socks[fd].fd < 0) return OB_EBADF;

    if (g_socks[fd].type == NET_SOCK_STREAM && g_socks[fd].tcp) {
        return tcp_send_data(g_socks[fd].tcp, buf, len);
    }
    if (g_socks[fd].type == NET_SOCK_DGRAM) {
        struct netif *nif = net_route_lookup(g_socks[fd].ns,
                                             g_socks[fd].remote_ip);
        if (!nif) return OB_ENETUNREACH;
        return udp_send(nif, g_socks[fd].remote_ip,
                        g_socks[fd].local_port,
                        g_socks[fd].remote_port,
                        buf, len) == 0 ? (int)len : -1;
    }
    return OB_ENOSYS;
}

/*
 * ★ 修复 3：net_recv 成功收到数据后唤醒 poll 等待者
 *
 * 人工必须审查：
 *   - 唤醒在 net_recv 成功返回数据之后调用；
 *   - select_notify_wakeup 内部会遍历 g_poll_waiters 并唤醒所有等待者；
 *   - 唤醒粒度粗（不精确匹配 fd），18D 阶段可接受。
 */
int net_recv(int fd, void *buf, uint32_t len, struct task_t *cur)
{
    int rc = net_check_task(cur);
    if (rc != 0) return rc;
    if (fd < 0 || fd >= NET_FD_MAX || g_socks[fd].fd < 0) return OB_EBADF;
    if (g_socks[fd].tcp) {
        int n = tcp_recv_data(g_socks[fd].tcp, buf, len);
        if (n > 0) select_notify_wakeup();
        return n;
    }
    return OB_ENOSYS;
}

int net_close(int fd, struct task_t *cur)
{
    int rc = net_check_task(cur);
    if (rc != 0) return rc;
    if (fd < 0 || fd >= NET_FD_MAX || g_socks[fd].fd < 0) return OB_EBADF;

    if (g_socks[fd].tcp) {
        tcp_close(g_socks[fd].tcp);
        g_socks[fd].tcp->used = 0;
        g_socks[fd].tcp = 0;
    }
    if (g_socks[fd].type == NET_SOCK_DGRAM && g_socks[fd].bound) {
        udp_unbind(g_socks[fd].ns, g_socks[fd].local_port);
    }
    g_socks[fd].fd = -1;
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/net/socket.c 结束===*/