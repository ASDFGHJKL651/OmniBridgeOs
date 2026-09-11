#include "kmalloc.h"
#include "pmm.h"
#include "printk.h"

#define NUM_CLASSES 9
static const size_t class_sizes[NUM_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048
};
static void *class_free[NUM_CLASSES];

static int class_of(size_t size)
{
    for (int i = 0; i < NUM_CLASSES; ++i)
        if (size <= class_sizes[i]) return i;
    return -1;
}

static void *refill(int ci)
{
    struct page *pg = pmm_alloc_pages(0);
    if (!pg) return 0;
    uint64_t phys = page_to_phys(pg);
    uint8_t *base = (uint8_t *)(uintptr_t)phys;
    size_t obj = class_sizes[ci];
    size_t n   = PAGE_SIZE / obj;

    pg->flags |= PG_SLAB;
    pg->slab_class = (uint16_t)(ci + 1); /* 0 表示非 slab */

    /* 把除第一个外的对象链入空闲链表 */
    for (size_t i = 1; i < n; ++i) {
        void *p = base + i * obj;
        *(void **)p = class_free[ci];
        class_free[ci] = p;
    }
    return base;
}

void kmalloc_init(void)
{
    for (int i = 0; i < NUM_CLASSES; ++i) class_free[i] = 0;
    printk("[KMALLOC] ready, %d size classes\n", NUM_CLASSES);
}

void *kmalloc(size_t size)
{
    if (size == 0) return 0;

    int ci = class_of(size);
    if (ci < 0) {
        /* > 2048：直接按页 */
        if (size > PAGE_SIZE) return 0; /* 本步骤只支持 ≤ 4KB 单页 */
        struct page *pg = pmm_alloc_pages(0);
        if (!pg) return 0;
        pg->slab_class = 0;
        return (void *)(uintptr_t)page_to_phys(pg);
    }

    if (!class_free[ci]) {
        if (!refill(ci)) return 0;
    }
    void *p = class_free[ci];
    class_free[ci] = *(void **)p;
    return p;
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
    uint16_t sc = pg->slab_class;
    if (sc == 0) {
        /* 整页分配：直接释放 */
        pmm_free_pages(pg, 0);
        return;
    }
    int ci = sc - 1;
    if (ci < 0 || ci >= NUM_CLASSES) return;
    *(void **)ptr = class_free[ci];
    class_free[ci] = ptr;
}

void *kmalloc_aligned(size_t size, size_t align)
{
    if (align <= 16) return kmalloc(size);
    /* 最简：取整页并向前偏移，本步骤只支持 align ≤ 4096 */
    if (size + align > PAGE_SIZE) return 0;
    struct page *pg = pmm_alloc_pages(0);
    if (!pg) return 0;
    uint64_t phys = page_to_phys(pg);
    uint64_t aligned = (phys + align - 1) & ~((uint64_t)align - 1);
    pg->slab_class = 0;
    return (void *)aligned;
}