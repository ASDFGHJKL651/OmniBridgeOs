/*===OmniBridgeOs/kernel/arch/x64/net/tcp.h===*/
#ifndef OMNIBRIDGE_NET_TCP_H
#define OMNIBRIDGE_NET_TCP_H

#include <stdint.h>
#include "net.h"
#include "netns.h"

#define TCP_HDR_MIN 20

/* TCP 状态 */
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RCVD     3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_FIN_WAIT_2   6
#define TCP_CLOSE_WAIT   7
#define TCP_CLOSING      8
#define TCP_LAST_ACK     9
#define TCP_TIME_WAIT   10

/* 标志位 */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_MAX_CONNS     NETNS_MAX_TCP_CONNS
#define TCP_RX_BUF_SIZE   2048
#define TCP_TX_BUF_SIZE   2048
#define TCP_RETX_TICKS    100     /* 1s @ 100Hz */
#define TCP_TXQ_MAX       4

struct tcp_tx_seg {
    uint32_t seq;
    uint32_t len;
    uint32_t sent;
    uint32_t retx;
    uint64_t last_tick;
    uint8_t  data[TCP_TX_BUF_SIZE];
    uint8_t  used;
};

struct tcp_conn {
    int      state;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;

    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint16_t snd_wnd;
    uint16_t rcv_wnd;

    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    uint32_t rx_len;

    /* ★ 发送队列与重传 */
    struct tcp_tx_seg txq[TCP_TXQ_MAX];
    uint32_t txq_count;

    /* ★ 拥塞控制占位接口 */
    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t rto_ticks;

    uint64_t last_tick;
    uint64_t retx_tick;

    void (*on_accept)(struct tcp_conn *c, void *arg);
    void (*on_recv)(struct tcp_conn *c, void *arg);
    void *cb_arg;

    struct netns *ns;
    uint8_t  used;
    uint8_t  _pad[3];
};

void tcp_init(void);

struct tcp_conn *tcp_listen(struct netns *ns, uint32_t local_ip,
                            uint16_t local_port);

struct tcp_conn *tcp_connect(struct netns *ns, uint32_t remote_ip,
                             uint16_t remote_port);

int  tcp_send_data(struct tcp_conn *c, const void *data, uint32_t len);
int  tcp_recv_data(struct tcp_conn *c, void *buf, uint32_t max_len);
void tcp_close(struct tcp_conn *c);

void tcp_rx(struct netif *nif, uint32_t src_ip, uint32_t dst_ip,
            struct netbuf *nb);

void tcp_tick(void);

void tcp_set_callbacks(struct tcp_conn *c,
                       void (*on_accept)(struct tcp_conn *, void *),
                       void (*on_recv)(struct tcp_conn *, void *),
                       void *arg);

/* ★ 拥塞控制接口占位 */
void tcp_congestion_init(struct tcp_conn *c);
void tcp_congestion_on_ack(struct tcp_conn *c, uint32_t acked);
void tcp_congestion_on_loss(struct tcp_conn *c);

#endif /* OMNIBRIDGE_NET_TCP_H */
/*===OmniBridgeOs/kernel/arch/x64/net/tcp.h 结束===*/