/*===OmniBridgeOs/kernel/arch/x64/net/firewall.h===*/
#ifndef OMNIBRIDGE_NET_FIREWALL_H
#define OMNIBRIDGE_NET_FIREWALL_H

#include <stdint.h>
#include "task.h"
#include "net.h"

#define FW_DIR_IN   0
#define FW_DIR_OUT  1

#define FW_PROTO_ANY 0
#define FW_PROTO_TCP 6
#define FW_PROTO_UDP 17
#define FW_PROTO_ICMP 1

#define FW_ACTION_ALLOW 0
#define FW_ACTION_DENY  1

struct fw_rule {
    uint8_t  dir;
    uint8_t  proto;
    uint8_t  action;
    uint8_t  _pad;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    struct fw_rule *next;
};

void fw_init(void);
int  fw_add_rule(struct netns *ns, uint8_t dir, uint8_t proto,
                 uint32_t src_ip, uint32_t dst_ip,
                 uint16_t src_port, uint16_t dst_port,
                 uint8_t action);
void fw_clear(struct netns *ns);

/* 钩子：允许返回 0，拒绝返回 -1（写入审计） */
int  fw_hook(struct netns *ns, uint8_t dir, uint8_t proto,
             uint32_t src_ip, uint32_t dst_ip,
             uint16_t src_port, uint16_t dst_port,
             struct task_t *cur);

void fw_dump(struct netns *ns);

#endif /* OMNIBRIDGE_NET_FIREWALL_H */
/*===OmniBridgeOs/kernel/arch/x64/net/firewall.h 结束===*/