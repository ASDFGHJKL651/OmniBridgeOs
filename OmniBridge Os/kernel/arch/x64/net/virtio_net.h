/*===OmniBridgeOs/kernel/arch/x64/net/virtio_net.h===*/
#ifndef OMNIBRIDGE_NET_VIRTIO_NET_H
#define OMNIBRIDGE_NET_VIRTIO_NET_H

#include "net.h"

/* 探测并初始化 VirtIO 网卡；成功返回 0 */
int virtio_net_init(struct netns *ns);

/* 轮询收包；由定时器调用 */
void virtio_net_poll(struct netif *nif);

/* ★ 别名接口：发送/接收 */
int  virtio_net_send(struct netif *nif, struct netbuf *nb);
void virtio_net_recv(struct netif *nif);

/* ★ v6：诊断用，转储 TX/RX 队列的 used->idx 进度。
 *   仅供调试与自检使用，不出现在生产数据路径。
 *   内部持有 g_lock，可与任意上下文安全并发调用。 */
void virtio_net_debug_dump(struct netif *nif);

#endif /* OMNIBRIDGE_NET_VIRTIO_NET_H */
/*===OmniBridgeOs/kernel/arch/x64/net/virtio_net.h 结束===*/