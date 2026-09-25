/*===OmniBridgeOs/kernel/arch/x64/net/netbuf.c===*/
#include "netbuf.h"
#include "kmalloc.h"
#include "serial.h"

#define NETBUF_POOL_MAX 128
static struct netbuf *g_pool[NETBUF_POOL_MAX];
static spinlock_t     g_pool_lock = SPINLOCK_INIT;
static int            g_inited = 0;

void netbuf_init(void)
{
    if (g_inited) return;
    spin_lock_init(&g_pool_lock);
    for (int i = 0; i < NETBUF_POOL_MAX; ++i) g_pool[i] = 0;
    g_inited = 1;
    serial_printf("[NETBUF] init: pool_size=%d data=%u head=%u tail=%u\n",
                  NETBUF_POOL_MAX, (unsigned)NETBUF_DATA_SIZE,
                  (unsigned)NETBUF_HEADROOM, (unsigned)NETBUF_TAILROOM);
}

static struct netbuf *pool_pop(void)
{
    uint64_t fl;
    spin_lock_irqsave(&g_pool_lock, &fl);
    struct netbuf *nb = 0;
    for (int i = 0; i < NETBUF_POOL_MAX; ++i) {
        if (g_pool[i]) { nb = g_pool[i]; g_pool[i] = 0; break; }
    }
    spin_unlock_irqrestore(&g_pool_lock, fl);
    return nb;
}

static int pool_push(struct netbuf *nb)
{
    uint64_t fl;
    spin_lock_irqsave(&g_pool_lock, &fl);
    int ok = 0;
    for (int i = 0; i < NETBUF_POOL_MAX; ++i) {
        if (!g_pool[i]) { g_pool[i] = nb; ok = 1; break; }
    }
    spin_unlock_irqrestore(&g_pool_lock, fl);
    return ok;
}

struct netbuf *netbuf_alloc(struct netns *ns, uint32_t size)
{
    if (!g_inited) netbuf_init();
    uint32_t total = NETBUF_HEADROOM + size + NETBUF_TAILROOM;
    if (total > NETBUF_DATA_SIZE) return 0;

    struct netbuf *nb = pool_pop();
    int from_pool = (nb != 0);
    if (!nb) {
        nb = (struct netbuf *)kzalloc(sizeof(*nb));
        if (!nb) return 0;
        nb->base = (uint8_t *)kzalloc(NETBUF_DATA_SIZE);
        if (!nb->base) { kfree(nb); return 0; }
        nb->base_cap = NETBUF_DATA_SIZE;
        spin_lock_init(&nb->lock);
    }

    /* 清零元数据字段 */
    nb->data     = nb->base + NETBUF_HEADROOM;
    nb->len      = 0;
    nb->cap      = size;
    nb->refcount = 1;
    nb->ns       = ns;

    if (!from_pool) {
        /* 首用时已经清零 buffer */
    }
    return nb;
}

struct netbuf *netbuf_clone(const struct netbuf *src)
{
    if (!src) return 0;
    struct netbuf *nb = netbuf_alloc(src->ns, src->len);
    if (!nb) return 0;
    for (uint32_t i = 0; i < src->len; ++i) nb->data[i] = src->data[i];
    nb->len = src->len;
    return nb;
}

void netbuf_retain(struct netbuf *nb)
{
    if (!nb) return;
    uint64_t fl;
    spin_lock_irqsave(&nb->lock, &fl);
    nb->refcount++;
    spin_unlock_irqrestore(&nb->lock, fl);
}

void netbuf_free(struct netbuf *nb)
{
    if (!nb) return;
    uint64_t fl;
    spin_lock_irqsave(&nb->lock, &fl);
    if (nb->refcount == 0) {
        spin_unlock_irqrestore(&nb->lock, fl);
        serial_printf("[NETBUF] WARN: double free, ignored\n");
        return;
    }
    nb->refcount--;
    uint32_t rc = nb->refcount;
    spin_unlock_irqrestore(&nb->lock, fl);
    if (rc > 0) return;

    /* 归还池；池满则释放 */
    nb->ns = 0;
    if (!pool_push(nb)) {
        kfree(nb->base);
        kfree(nb);
    }
}

int netbuf_push(struct netbuf *nb, uint32_t bytes)
{
    if (!nb) return -1;
    if (nb->data - nb->base < (long)bytes) return -1;
    nb->data -= bytes;
    nb->len  += bytes;
    nb->cap  += bytes;
    return 0;
}

int netbuf_put(struct netbuf *nb, uint32_t bytes)
{
    if (!nb) return -1;
    if (nb->len + bytes > nb->cap + (nb->cap - nb->len)) return -1;
    /* 简化：直接扩大 len，要求 cap 足够 */
    if (nb->len + bytes > nb->base_cap - (uint32_t)(nb->data - nb->base)) return -1;
    nb->len += bytes;
    return 0;
}

void netbuf_pull(struct netbuf *nb, uint32_t bytes)
{
    if (!nb) return;
    if (bytes > nb->len) bytes = nb->len;
    nb->data += bytes;
    nb->len  -= bytes;
    nb->cap  -= bytes;
}

uint32_t netbuf_headroom(const struct netbuf *nb)
{
    if (!nb) return 0;
    return (uint32_t)(nb->data - nb->base);
}

uint32_t netbuf_tailroom(const struct netbuf *nb)
{
    if (!nb) return 0;
    uint32_t used = (uint32_t)(nb->data - nb->base) + nb->len;
    return nb->base_cap - used;
}
/*===OmniBridgeOs/kernel/arch/x64/net/netbuf.c 结束===*/