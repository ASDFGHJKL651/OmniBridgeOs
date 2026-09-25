/*===OmniBridgeOs/kernel/arch/x64/net/firewall.c===*/
#include "firewall.h"
#include "kmalloc.h"
#include "serial.h"
#include "audit.h"
#include "permission.h"

void fw_init(void)
{
    serial_printf("[FIREWALL] init: hooks installed\n");
}

int fw_add_rule(struct netns *ns, uint8_t dir, uint8_t proto,
                uint32_t src_ip, uint32_t dst_ip,
                uint16_t src_port, uint16_t dst_port,
                uint8_t action)
{
    if (!ns) return -1;
    struct fw_rule *r = (struct fw_rule *)kzalloc(sizeof(*r));
    if (!r) return -1;
    r->dir = dir; r->proto = proto; r->action = action;
    r->src_ip = src_ip; r->dst_ip = dst_ip;
    r->src_port = src_port; r->dst_port = dst_port;
    r->next = 0;

    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    r->next = ns->fw_rules;
    ns->fw_rules = r;
    ns->fw_count++;
    spin_unlock_irqrestore(&ns->lock, fl);
    return 0;
}

void fw_clear(struct netns *ns)
{
    if (!ns) return;
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    struct fw_rule *r = ns->fw_rules;
    ns->fw_rules = 0;
    ns->fw_count = 0;
    spin_unlock_irqrestore(&ns->lock, fl);
    while (r) { struct fw_rule *n = r->next; kfree(r); r = n; }
}

int fw_hook(struct netns *ns, uint8_t dir, uint8_t proto,
            uint32_t src_ip, uint32_t dst_ip,
            uint16_t src_port, uint16_t dst_port,
            struct task_t *cur)
{
    if (!ns) return 0;

    /* 沙盒命名空间：任何外部通信一律拒绝（loopback 由调用者自行放行） */
    if (ns->is_sandbox) {
        uint8_t a = (uint8_t)((dst_ip >> 24) & 0xFF);
        if (a != 127) {
            uint64_t pid = cur ? cur->pid : 0;
            audit_event(AUDIT_EV_SANDBOX_ESCAPE, AUDIT_LVL_CRITICAL,
                        pid, proto, dst_ip, dir, "(net-sandbox-block)");
            serial_printf("[FIREWALL] SANDBOX BLOCK pid=%llu proto=%u dst=0x%08x\n",
                          (unsigned long long)pid, (unsigned)proto,
                          (unsigned)dst_ip);
            return -1;
        }
    }

    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    for (struct fw_rule *r = ns->fw_rules; r; r = r->next) {
        if (r->dir != dir && r->dir != 0xFF) continue;
        if (r->proto != FW_PROTO_ANY && r->proto != proto) continue;
        if (r->src_ip && r->src_ip != src_ip) continue;
        if (r->dst_ip && r->dst_ip != dst_ip) continue;
        if (r->src_port && r->src_port != src_port) continue;
        if (r->dst_port && r->dst_port != dst_port) continue;
        uint8_t act = r->action;
        spin_unlock_irqrestore(&ns->lock, fl);
        if (act == FW_ACTION_DENY) {
            uint64_t pid = cur ? cur->pid : 0;
            audit_event(AUDIT_EV_COMPAT_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                        pid, proto, dst_ip, dir, "(firewall-deny)");
            return -1;
        }
        return 0;
    }
    spin_unlock_irqrestore(&ns->lock, fl);
    return 0;
}

void fw_dump(struct netns *ns)
{
    if (!ns) return;
    serial_printf("[FIREWALL] dump ns=%u:\n", (unsigned)ns->id);
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    for (struct fw_rule *r = ns->fw_rules; r; r = r->next) {
        serial_printf("  dir=%u proto=%u src=0x%08x dst=0x%08x "
                      "sp=%u dp=%u act=%u\n",
                      (unsigned)r->dir, (unsigned)r->proto,
                      (unsigned)r->src_ip, (unsigned)r->dst_ip,
                      (unsigned)r->src_port, (unsigned)r->dst_port,
                      (unsigned)r->action);
    }
    spin_unlock_irqrestore(&ns->lock, fl);
}
/*===OmniBridgeOs/kernel/arch/x64/net/firewall.c 结束===*/