#include "kmalloc.h"
#include "slab.h"
#include "pmm.h"
#include "printk.h"
#include "serial.h"

/*
 * kmalloc —— 构建在 SLAB 上的通用分配器。
 *
 * 关键决策（人工必须审查）：
 *   - 大小类与原来一致：8/16/32/64/128/256/512/1024/2048。
 *   - 每个大小类对应一个 kmem_cache，cache 的 object_size 就等于该类
 *     大小，align 固定为 8。
 *   - 超过 2048 但 <= PAGE_SIZE 的请求直接走伙伴系统分配单页，并清除
 *     PG_SLAB 标志；kfree 通过 PG_SLAB 判断是否是 slab 对象。
 *   - kmalloc_aligned 保持原实现（整页 + 对齐偏移），不支持跨页。
 */

#define NUM_CLASSES 9
static const size_t class_sizes[NUM_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};
static struct kmem_cache *class_cache[NUM_CLASSES];

static int class_of(size_t size)
{
    for (int i = 0; i < NUM_CLASSES; ++i)
        if (size <= class_sizes[i]) return i;
    return -1;
}

void kmalloc_init(void)
{
    slab_init();

    for (int i = 0; i < NUM_CLASSES; ++i) {
        /* 构造形如 "kmalloc-8" / "kmalloc-1024" 的名字 */
        char name[SLAB_MAX_NAME];
        const char *pfx = "kmalloc-";
        int n = 0;
        while (pfx[n] && n + 1 < SLAB_MAX_NAME) { name[n] = pfx[n]; ++n; }

        /* 将 class_sizes[i] 的十进制表示追加到 name */
        size_t v = class_sizes[i];
        char num[16];
        int nd = 0;
        if (v == 0) num[nd++] = '0';
        while (v && nd < (int)sizeof(num) - 1) {
            num[nd++] = (char)('0' + (v % 10));
            v /= 10;
        }
        /* num 是反的，反转写入 */
        for (int k = nd - 1; k >= 0 && n + 1 < SLAB_MAX_NAME; --k)
            name[n++] = num[k];
        name[n] = '\0';

        class_cache[i] = kmem_cache_create(name, class_sizes[i], 8, 0);
    }

    printk("[KMALLOC] ready, %d size classes (backed by SLAB)\n",
           NUM_CLASSES);
}

void *kmalloc(size_t size)
{
    if (size == 0) return 0;

    int ci = class_of(size);
    if (ci >= 0) {
        return kmem_cache_alloc(class_cache[ci]);
    }

    /* > 2048：直接按单页分配（本步只支持 <= 4KB） */
    if (size > PAGE_SIZE) return 0;
    struct page *pg = pmm_alloc_pages(0);
    if (!pg) return 0;
    /* 清除非 slab 标志，避免 kfree 走 slab 路径 */
    pg->flags     &= (uint8_t)~PG_SLAB;
    pg->slab_cache = 0;
    pg->slab_free  = 0;
    return (void *)(uintptr_t)page_to_phys(pg);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (!p) return 0;
    uint8_t *b = (uint8_t *)p;
    for (size_t i = 0; i < size; ++i) b[i] = 0;
    return p;
}

void kfree(void *ptr)
{
    if (!ptr) return;

    uint64_t v = (uint64_t)(uintptr_t)ptr;
    struct page *pg = phys_to_page(v & PAGE_MASK);

    if (pg->flags & PG_SLAB) {
        struct kmem_cache *c = (struct kmem_cache *)pg->slab_cache;
        if (!c) {
            printk("[KMALLOC] kfree: PG_SLAB with NULL cache\n");
            return;
        }
        kmem_cache_free(c, ptr);
        return;
    }

    /* 整页分配：order 记录在页头 */
    int order = pg->order;
    pmm_free_pages(pg, order);
}

void *kmalloc_aligned(size_t size, size_t align)
{
    if (align <= 16) return kmalloc(size);
    if (size + align > PAGE_SIZE) return 0;

    struct page *pg = pmm_alloc_pages(0);
    if (!pg) return 0;

    uint64_t phys    = page_to_phys(pg);
    uint64_t aligned = (phys + align - 1) & ~((uint64_t)align - 1);

    /* 整页分配，非 slab */
    pg->flags      &= (uint8_t)~PG_SLAB;
    pg->slab_cache  = 0;
    pg->slab_free   = 0;

    return (void *)aligned;
}