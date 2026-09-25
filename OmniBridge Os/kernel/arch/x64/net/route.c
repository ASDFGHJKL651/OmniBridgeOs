/*===OmniBridgeOs/kernel/arch/x64/net/route.c===*/
#include "route.h"
#include "kmalloc.h"
#include "serial.h"

void route_init(void)
{
    serial_printf("[ROUTE] init: longest-prefix matching\n");
}

int route_add(struct netns *ns, uint32_t dst, uint32_t mask,
              uint32_t gw, struct netif *nif, uint8_t flags)
{
    if (!ns) return -1;

    struct route_entry *e = (struct route_entry *)kzalloc(sizeof(*e));
    if (!e) return -1;
    e->dst     = dst;
    e->mask    = mask;
    e->gateway = gw;
    e->nif     = nif;
    e->flags   = flags;
    e->next    = 0;

    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    e->next     = ns->routes;
    ns->routes  = e;
    ns->route_count++;
    spin_unlock_irqrestore(&ns->lock, fl);

    serial_printf("[ROUTE] add dst=0x%x mask=0x%x gw=0x%x iface=%s\n",
                  (unsigned)dst, (unsigned)mask, (unsigned)gw,
                  nif ? nif->name : "(null)");
    return 0;
}

void route_clear(struct netns *ns)
{
    if (!ns) return;
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    struct route_entry *e = ns->routes;
    ns->routes = 0;
    ns->route_count = 0;
    spin_unlock_irqrestore(&ns->lock, fl);
    while (e) {
        struct route_entry *n = e->next;
        kfree(e);
        e = n;
    }
}

int route_lookup(struct netns *ns, uint32_t dst, struct route_entry **out)
{
    if (!ns || !out) return -1;
    *out = 0;

    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);

    struct route_entry *best = 0;
    uint32_t best_mask = 0;
    for (struct route_entry *e = ns->routes; e; e = e->next) {
        if ((dst & e->mask) == (e->dst & e->mask)) {
            if (!best || e->mask > best_mask ||
                (e->mask == best_mask && (e->flags & RT_FLAG_HOST))) {
                best = e; best_mask = e->mask;
            }
        }
    }
    if (best) *out = best;
    spin_unlock_irqrestore(&ns->lock, fl);
    return best ? 0 : -1;
}

void route_dump(struct netns *ns)
{
    if (!ns) return;
    serial_printf("[ROUTE] dump ns=%u:\n", (unsigned)ns->id);
    uint64_t fl;
    spin_lock_irqsave(&ns->lock, &fl);
    for (struct route_entry *e = ns->routes; e; e = e->next) {
        serial_printf("  dst=0x%08x mask=0x%08x gw=0x%08x if=%s flags=0x%x\n",
                      (unsigned)e->dst, (unsigned)e->mask,
                      (unsigned)e->gateway,
                      e->nif ? e->nif->name : "(null)",
                      (unsigned)e->flags);
    }
    spin_unlock_irqrestore(&ns->lock, fl);
}
/*===OmniBridgeOs/kernel/arch/x64/net/route.c 结束===*/