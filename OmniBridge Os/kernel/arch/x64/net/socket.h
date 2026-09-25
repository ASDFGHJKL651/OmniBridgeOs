/*===OmniBridgeOs/kernel/arch/x64/net/socket.h===*/
#ifndef OMNIBRIDGE_NET_SOCKET_H
#define OMNIBRIDGE_NET_SOCKET_H

#include <stdint.h>
#include "net.h"
#include "task.h"

#define NET_AF_INET   2
#define NET_AF_INET6  10
#define NET_SOCK_STREAM 1
#define NET_SOCK_DGRAM  2

#define NET_FD_MAX 32

struct net_socket {
    int      fd;
    int      domain;
    int      type;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_ip;
    uint32_t remote_ip;
    struct tcp_conn *tcp;
    uint8_t  bound;
    uint8_t  listening;
    uint8_t  connected;
    uint8_t  _pad;
    struct netns *ns;
};

void net_socket_init(void);

int  net_socket(int domain, int type, int protocol, struct task_t *cur);
int  net_bind(int fd, uint32_t ip, uint16_t port, struct task_t *cur);
int  net_listen(int fd, int backlog, struct task_t *cur);
int  net_accept(int fd, struct task_t *cur);
int  net_connect(int fd, uint32_t ip, uint16_t port, struct task_t *cur);
int  net_send(int fd, const void *buf, uint32_t len, struct task_t *cur);
int  net_recv(int fd, void *buf, uint32_t len, struct task_t *cur);
int  net_close(int fd, struct task_t *cur);

#endif /* OMNIBRIDGE_NET_SOCKET_H */
/*===OmniBridgeOs/kernel/arch/x64/net/socket.h 结束===*/