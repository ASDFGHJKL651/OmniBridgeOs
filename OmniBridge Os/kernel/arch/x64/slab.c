#include "slab.h"
#include "pmm.h"
#include "printk.h"
#include "serial.h"

/*
 * SLAB 分配器实现。
 *
 * 关键不变量（人工必须审查）：
 *   1) 对于每一个 PG_SLAB 的页 pg：
 *        pg->slab_cache 指向一个已注册的 kmem_cache
 *        pg->slab_free  要么为 NULL（满 slab），要么指向一个空闲对象链
 *        pg->refcount   等于当前已分配对象数（0 .. objects_per_slab）
 *   2) 对于 cache c：c->partial 链表中的每一个 pg 都满足
 *        0 <= pg->refcount < pg->objects_per_slab
 *        且 pg->slab_free != NULL
 *   3) 一个 pg 只能出现在一个 cache 的 partial 链表中。
 *
 * 并发保护：
 *   - 所有对 c->partial 与页面字段的修改都在 c->lock 内。
 *   - 页属于哪个 cache 由页面字段决定；从任意指针 kfree 时不需
 *     要 cache 参数，直接通过页面查找（见 kmalloc.c）。
 *
 * 注意：
 *   - SLAB 的「slab 基址」是 page_to_phys(pg)，即恒等映射下的物理地址。
 *     依赖 pmm.c 的恒等映射（vmm.c 中已建立 0..4GB 恒等映射）。
 */

static struct kmem_cache g_cache_pool[SLAB_MAX_CACHES];
static int g_cache_count = 0;
static int g_slab_inited = 0;

void slab_init(void)
{
    if (g_slab_inited) return;
    g_cache_count = 0;
    for (int i = 0; i < SLAB_MAX_CACHES; ++i) {
        g_cache_pool[i].magic = 0;
    }
    g_slab_inited = 1;
    serial_printf("[SLAB] initialized, cache pool=%d\n", SLAB_MAX_CACHES);
}

static size_t round_up(size_t v, size_t a)
{
    if (a == 0) a = 8;
    return (v + a - 1) & ~(a - 1);
}

static void str_copy_trunc(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    if (n == 0) return;
    while (src && src[i] && i + 1 < n) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/* 计算 slab 布局：返回 0 成功、-1 失败 */
static int calc_layout(size_t obj_size, size_t *out_objs, size_t *out_order)
{
    size_t min_obj = obj_size;
    if (min_obj < sizeof(void *)) min_obj = sizeof(void *);

    for (int order = 0; order < MAX_ORDER; ++order) {
        size_t bytes = PAGE_SIZE << order;
        size_t nobj  = bytes / min_obj;
        if (nobj >= 2) {
            /* 每个 slab 里对象平均占用不能低于 min_obj */
            *out_objs  = nobj;
            *out_order = (size_t)order;
            return 0;
        }
    }
    return -1;
}

/* 从伙伴系统申请一个新 slab 并初始化：
 *   - 标记 PG_SLAB
 *   - 建立完整的空闲对象链表
 *   - refcount = 0
 * 返回 slab 页头，失败返回 NULL。调用者需持有 c->lock。 */
static struct page *slab_new(struct kmem_cache *c)
{
    struct page *pg = pmm_alloc_pages((int)c->slab_order);
    if (!pg) return 0;

    uint8_t *base = (uint8_t *)(uintptr_t)page_to_phys(pg);
    size_t   osz  = c->object_size;
    size_t   nobj = c->objects_per_slab;

    /* 建立空闲链表：obj[0] -> obj[1] -> ... -> obj[nobj-1] -> NULL */
    void *head = 0;
    for (size_t i = nobj; i-- > 0; ) {
        uint8_t *p = base + i * osz;
        *(void **)p = head;
        head = p;
    }

    pg->flags      |= PG_SLAB;
    pg->slab_cache  = c;
    pg->slab_free   = head;
    pg->refcount    = 0;
    pg->next        = 0;

    return pg;
}

/* 将 slab 从 cache 的 partial 链表中摘除。调用者需持有 c->lock。 */
static void partial_remove(struct kmem_cache *c, struct page *pg)
{
    struct page **pp = &c->partial;
    while (*pp && *pp != pg) pp = &(*pp)->next;
    if (*pp == pg) {
        *pp = pg->next;
        pg->next = 0;
    }
}

struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                     size_t align, uint32_t flags)
{
    (void)flags;
    if (!g_slab_inited) slab_init();

    if (size == 0) return 0;
    if (g_cache_count >= SLAB_MAX_CACHES) {
        printk("[SLAB] cache pool exhausted\n");
        return 0;
    }

    size_t a = (align < sizeof(void *)) ? sizeof(void *) : align;
    size_t osz = round_up(size, a);
    if (osz < sizeof(void *)) osz = sizeof(void *);

    size_t nobj = 0;
    size_t order = 0;
    if (calc_layout(osz, &nobj, &order) < 0) {
        printk("[SLAB] cannot fit object size %llu\n",
               (unsigned long long)osz);
        return 0;
    }

    struct kmem_cache *c = &g_cache_pool[g_cache_count++];
    str_copy_trunc(c->name, name, SLAB_MAX_NAME);
    c->object_size      = osz;
    c->objects_per_slab = nobj;
    c->slab_order       = order;
    c->partial          = 0;
    spin_lock_init(&c->lock);
    c->magic            = SLAB_MAGIC;

    serial_printf("[SLAB] cache '%s' obj=%llu n=%llu order=%llu\n",
                  c->name,
                  (unsigned long long)c->object_size,
                  (unsigned long long)c->objects_per_slab,
                  (unsigned long long)c->slab_order);
    return c;
}

void *kmem_cache_alloc(struct kmem_cache *c)
{
    if (!c || c->magic != SLAB_MAGIC) return 0;

    uint64_t flags;
    spin_lock_irqsave(&c->lock, &flags);

    struct page *pg = c->partial;

    /* 若没有可用的 partial slab，则从伙伴系统申请一个 */
    if (!pg) {
        pg = slab_new(c);
        if (!pg) {
            spin_unlock_irqrestore(&c->lock, flags);
            return 0;
        }
        /* 加入 partial 链表头部 */
        pg->next   = c->partial;
        c->partial = pg;
    }

    /* 从空闲链表弹出一个对象 */
    void *obj = pg->slab_free;
    pg->slab_free = *(void **)obj;
    pg->refcount += 1;

    /* 若 slab 变满，则从 partial 摘除（保持 partial 语义） */
    if (pg->refcount >= c->objects_per_slab) {
        partial_remove(c, pg);
    }

    spin_unlock_irqrestore(&c->lock, flags);
    return obj;
}

void kmem_cache_free(struct kmem_cache *c, void *obj)
{
    if (!c || c->magic != SLAB_MAGIC || !obj) return;

    uint64_t v = (uint64_t)(uintptr_t)obj;
    struct page *pg = phys_to_page(v & PAGE_MASK);

    /* 一致性检查：页必须是该 cache 的 slab 页 */
    if (!(pg->flags & PG_SLAB) || pg->slab_cache != c) {
        printk("[SLAB] free: object not owned by cache '%s'\n", c->name);
        return;
    }

    uint64_t flags;
    spin_lock_irqsave(&c->lock, &flags);

    int was_full = (pg->refcount >= c->objects_per_slab);

    /* 把对象压回空闲链表 */
    *(void **)obj = pg->slab_free;
    pg->slab_free = obj;
    pg->refcount -= 1;

    /* 若刚从「满」变成「非满」，则重新加入 partial 链表 */
    if (was_full) {
        pg->next   = c->partial;
        c->partial = pg;
    }

    /* 若 slab 现在完全为空，则归还给伙伴系统（保留 partial 头部一个，
     * 以避免抖动；这里采取简单策略：只要不为空就留在 partial 中） */
    if (pg->refcount == 0) {
        /* 如果当前 partial 只有一个页且它正是 pg，就保留它作为缓冲 */
        struct page *kept = c->partial;
        if (kept != pg || kept->next != 0) {
            partial_remove(c, pg);
            pg->flags &= (uint8_t)~PG_SLAB;
            pg->slab_cache = 0;
            pg->slab_free  = 0;
            pg->refcount   = 0;
            pmm_free_pages(pg, (int)c->slab_order);
        }
    }

    spin_unlock_irqrestore(&c->lock, flags);
}

void kmem_cache_destroy(struct kmem_cache *c)
{
    if (!c || c->magic != SLAB_MAGIC) return;

    uint64_t flags;
    spin_lock_irqsave(&c->lock, &flags);

    /* 释放所有 partial 中的 slab（满 slab 本步不做追踪） */
    struct page *pg = c->partial;
    c->partial = 0;
    while (pg) {
        struct page *next = pg->next;
        pg->next       = 0;
        pg->flags     &= (uint8_t)~PG_SLAB;
        pg->slab_cache = 0;
        pg->slab_free  = 0;
        pg->refcount   = 0;
        pmm_free_pages(pg, (int)c->slab_order);
        pg = next;
    }

    c->magic = 0;
    spin_unlock_irqrestore(&c->lock, flags);
}