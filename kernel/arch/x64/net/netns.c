/*===OmniBridgeOs/kernel/arch/x64/net/netns.c===*/
#include "netns.h"
#include "kmalloc.h"
#include "serial.h"

static struct netns g_default_ns;
static uint32_t     g_next_id = 1;
static int          g_inited = 0;

static void netns_init_tables(struct netns *ns)
{
    if (!ns) return;

    for (uint32_t i = 0; i < NETNS_MAX_UDP_BINDINGS; ++i) {
        ns->udp_binds[i] = 0;
    }
    ns->udp_count = 0;
    spin_lock_init(&ns->udp_lock);

    for (uint32_t i = 0; i < NETNS_MAX_TCP_CONNS; ++i) {
        ns->tcp_conns[i] = 0;
    }
    ns->tcp_count = 0;
    spin_lock_init(&ns->tcp_lock);
}

void netns_init(void)
{
    if (g_inited) return;

    uint8_t *p = (uint8_t *)&g_default_ns;
    for (unsigned i = 0; i < sizeof(g_default_ns); ++i) p[i] = 0;
    g_default_ns.id = 0;
    const char *nm = "default";
    int i = 0; while (nm[i] && i < 31) { g_default_ns.name[i] = nm[i]; ++i; }
    g_default_ns.name[i] = '\0';
    g_default_ns.iface_count = 0;
    g_default_ns.route_count = 0;
    g_default_ns.fw_count    = 0;
    g_default_ns.is_sandbox  = 0;
    spin_lock_init(&g_default_ns.lock);

    netns_init_tables(&g_default_ns);

    g_next_id = 1;
    g_inited  = 1;
    serial_printf("[NETNS] init: default ns created\n");
}

struct netns *netns_default(void)
{
    if (!g_inited) netns_init();
    return &g_default_ns;
}

struct netns *netns_create(const char *name, uint8_t is_sandbox)
{
    if (!g_inited) netns_init();

    struct netns *ns = (struct netns *)kzalloc(sizeof(*ns));
    if (!ns) return 0;

    ns->id         = g_next_id++;
    ns->iface_count = 0;
    ns->route_count = 0;
    ns->fw_count    = 0;
    ns->is_sandbox  = is_sandbox;
    spin_lock_init(&ns->lock);

    netns_init_tables(ns);

    int i = 0;
    if (name) { while (name[i] && i < 31) { ns->name[i] = name[i]; ++i; } }
    ns->name[i] = '\0';

    serial_printf("[NETNS] create id=%u name=%s sandbox=%u\n",
                  (unsigned)ns->id, ns->name, (unsigned)is_sandbox);
    return ns;
}

void netns_destroy(struct netns *ns)
{
    if (!ns || ns == &g_default_ns) return;

    /*
     * 人工必须审查：
     *   当前不释放 udp_binds / tcp_conns 中可能存在的动态对象。
     *   若后续允许销毁非默认 netns，必须在此调用 udp_unbind/tcp_close
     *   或提供专门的 netns 清理接口。
     */
    kfree(ns);
}

void netns_add_iface(struct netns *ns, struct netif *nif)
{
    if (!ns || !nif) return;
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    if (ns->iface_count < NETNS_MAX_IFACES) {
        ns->ifaces[ns->iface_count++] = nif;
    }
    spin_unlock_irqrestore(&ns->lock, fl);
}
/*===OmniBridgeOs/kernel/arch/x64/net/netns.c 结束===*/