#include "stress.h"
#include "pmm.h"
#include "slab.h"
#include "kmalloc.h"
#include "printk.h"
#include "serial.h"

/*
 * 压力测试实现。
 *
 * 设计约束（人工必须审查）：
 *   1) slab.c 的 cache 池当前为静态分配（SLAB_MAX_CACHES=64），
 *      kmem_cache_destroy 不归还池槽位。因此每次 create/destroy 都会
 *      消耗一个池槽位；FULL / LONG 中要限制反复 create/destroy 的次数。
 *      这里保守地限制为 50 次，并留作未来改进点（归还池槽位）。
 *   2) slab.c 采用「每个 cache 保留 1 个空 slab 作缓冲」的策略：
 *      第一次被触碰的 cache 会「永久」占用 1 个物理页。
 *      因此任何「绝对基线」比较之前，必须先对所有 kmalloc 大小类
 *      做一次预热，把每个 cache 的首个缓冲 slab 建立起来。
 *      这就是 warmup_all_caches() 存在的原因。
 *   3) 稳态判据只比较第 1、2 轮（而非 0、1、2 轮），原因同上：
 *      第 0 轮通常会触发若干 cache 的首次缓冲分配。
 *   4) 所有分配失败路径都做资源回收。
 *
 * 目标 ABI 约束（x86_64-pc-win32-coff）：
 *   - 栈上分配 > 4KB 的局部变量会触发 __chkstk；-ffreestanding 环境
 *     没有它的实现。因此所有大数组使用 static 存储（放在 .bss）。
 *   - `= {0}` 初始化在大数组上会生成 memset 调用；同样没有实现。
 *     所以全部改为显式清零循环。
 *   - static 缓冲区在函数间共享：本文件的调用链是单线程串行执行
 *     （_kstart_c 主线程 → stress_run → 具体段/轮次），不会并发进入
 *     同一缓冲区，因此 static 复用是安全的。
 *
 * printk 约束（人工必须审查）：
 *   - kprintf.c 里的 ob_vsnprintf 不支持 `%-` 左对齐标志。本文件所有
 *     格式串都避免使用 `-`，以免参数错位（这是一个已踩过的坑：错位
 *     会让 %llu 拿到 tag、%lld 拿到 now，日志不可读）。
 */

/* ---------- 工具 ---------- */

static uint64_t free_pages_now(void)
{
    return pmm_free_pages_count();
}

static void print_delta(const char *tag, uint64_t base, uint64_t now)
{
    long long d = (long long)now - (long long)base;
    /* 注意：不使用 %-6s，见文件头 printk 约束 */
    printk("[STRESS] %s free=%llu delta=%lld\n",
           tag,
           (unsigned long long)now,
           d);
}

/* 确定性 LCG，避免引入 libc */
static inline uint32_t lcg_next(uint32_t *s)
{
    *s = (*s) * 1103515245u + 12345u;
    return *s;
}

/* 手动清零 void* 数组，避免 memset */
static void clear_ptrs(void **p, int n)
{
    for (int i = 0; i < n; ++i) p[i] = 0;
}

/*
 * 预热：对每个 kmalloc 大小类做一次分配/释放。
 *
 * 目的：让 slab.c 「每 cache 保留 1 个缓冲 slab」的一次性开销
 *       发生在基线快照之前。之后无论怎么抖动，只要对象全部释放，
 *       free_count 都应回到这个基线。
 *
 * 成本：9 次 kmalloc/kfree，最多消耗 9 个物理页（如果缓存此前全是冷的）。
 * 幂等性：对已经预热过的 cache，第二次调用不再分配新页。
 */
static void warmup_all_caches(void)
{
    for (size_t s = 8; s <= 2048; s *= 2) {
        void *p = kmalloc(s);
        if (p) kfree(p);
    }
}

/* ---------- BASIC ---------- */

static int run_basic(void)
{
    printk("[STRESS] level=BASIC\n");

    /* 1) 大小类遍历：命中 9 个 cache，不做绝对基线比较 */
    for (size_t s = 1; s <= 2048; s *= 2) {
        void *t = kmalloc(s);
        if (!t) {
            printk(KERN_ERR "[STRESS] kmalloc(%llu) failed\n",
                   (unsigned long long)s);
            return -1;
        }
        kfree(t);
    }

    /* 2) 用户 cache 单次 create/use/destroy */
    struct kmem_cache *c = kmem_cache_create("ob_selftest", 96, 8, 0);
    if (!c) {
        printk(KERN_ERR "[STRESS] kmem_cache_create failed\n");
        return -1;
    }

    void *objs[64];
    clear_ptrs(objs, 64);

    int n = 0;
    for (; n < 64; ++n) {
        objs[n] = kmem_cache_alloc(c);
        if (!objs[n]) {
            printk(KERN_ERR "[STRESS] kmem_cache_alloc failed at %d\n", n);
            for (int j = 0; j < n; ++j) kmem_cache_free(c, objs[j]);
            kmem_cache_destroy(c);
            return -1;
        }
    }
    for (int i = 0; i < n; ++i) kmem_cache_free(c, objs[i]);
    kmem_cache_destroy(c);

    /* 3) 200 轮混合大小 */
    void *batch[32];
    clear_ptrs(batch, 32);

    for (int round = 0; round < 200; ++round) {
        int i = 0;
        for (; i < 32; ++i) {
            batch[i] = kmalloc((size_t)(8u << (i & 7)));
            if (!batch[i]) {
                printk(KERN_ERR "[STRESS] alloc failed round=%d\n", round);
                for (int j = 0; j < i; ++j) kfree(batch[j]);
                return -1;
            }
        }
        for (i = 0; i < 32; ++i) kfree(batch[i]);
    }

    printk("[STRESS] BASIC OK\n");
    return 0;
}

/* ---------- STEADY ---------- */

static int steady_round(int seed_idx, uint64_t *out_free, int *oom)
{
    /* 128 * 8 = 1024 字节，仍可安全放栈；static 保持统一风格 */
    static void *slots[128];

    clear_ptrs(slots, 128);
    *oom = 0;

    uint32_t seed = 0xC0FFEE00u ^ (uint32_t)seed_idx;
    for (int op = 0; op < 4000; ++op) {
        uint32_t r = lcg_next(&seed);
        int idx = (int)(r % 128u);
        if (slots[idx]) {
            kfree(slots[idx]);
            slots[idx] = 0;
        } else {
            size_t sz = (size_t)8u << (r % 9u);  /* 8..2048 */
            slots[idx] = kmalloc(sz);
            if (!slots[idx]) *oom = 1;
        }
    }
    for (int i = 0; i < 128; ++i)
        if (slots[i]) kfree(slots[i]);

    *out_free = free_pages_now();
    return 0;
}

static int steady_internal(void)
{
    /* 预热：保证每个 cache 的缓冲 slab 在第 0 轮之前就建立。
     * 这样轮次之间的比较才只反映「分配/释放是否守恒」。 */
    warmup_all_caches();

    uint64_t r[3];
    for (int i = 0; i < 3; ++i) {
        int oom = 0;
        steady_round(i, &r[i], &oom);
        printk("[STRESS] steady round %d free=%llu%s\n",
               i,
               (unsigned long long)r[i],
               oom ? " (OOM in round)" : "");
        if (oom) {
            printk(KERN_ERR "[STRESS] OOM in steady round %d\n", i);
            return -1;
        }
    }
    /* 严格判据：轮 1 与轮 2 必须完全相等 */
    if (r[1] != r[2]) {
        printk(KERN_ERR "[STRESS] NOT steady: r1=%llu r2=%llu\n",
               (unsigned long long)r[1],
               (unsigned long long)r[2]);
        return -1;
    }
    return 0;
}

static int run_steady(void)
{
    printk("[STRESS] level=STEADY\n");
    int rc = steady_internal();
    if (rc == 0) printk("[STRESS] STEADY OK\n");
    return rc;
}

/* ---------- FULL 分段 ---------- */

/* A) 单调分配 512 个 64B 对象后一次性释放。
 *    pool[512] = 4096 字节，必须 static 以避免 __chkstk。 */
static void segment_a(uint64_t base)
{
    static void *pool[512];
    clear_ptrs(pool, 512);

    int n = 0;
    for (; n < 512; ++n) {
        pool[n] = kmalloc(64);
        if (!pool[n]) break;
    }
    printk("[STRESS] A: allocated %d x 64B\n", n);
    for (int i = 0; i < n; ++i) kfree(pool[i]);
    print_delta("A", base, free_pages_now());
}

/* B) 5000 次随机大小交错操作，破坏伙伴顺序。
 *    这是唯一会触及全部 9 个 kmalloc cache 的段，因此若预热缺失，
 *    B 段结束后 free 会整体下跌（每 cache 预留 1 个 slab）。 */
static void segment_b(uint64_t base)
{
    static void *slots[256];
    clear_ptrs(slots, 256);

    uint32_t seed = 0x12345678u;
    for (int op = 0; op < 5000; ++op) {
        uint32_t r = lcg_next(&seed);
        int idx = (int)(r % 256u);
        if (slots[idx]) {
            kfree(slots[idx]);
            slots[idx] = 0;
        } else {
            size_t sz = (size_t)8u << (r % 9u);
            slots[idx] = kmalloc(sz);
        }
    }
    for (int i = 0; i < 256; ++i)
        if (slots[i]) kfree(slots[i]);
    print_delta("B", base, free_pages_now());
}

/* C) 用户 cache 反复 create/destroy（限 50 次，见文件头说明）。
 *    每次 create/use/destroy 都会拿 1 页、还 1 页，理论上净零；
 *    cache 池槽位不归还，但物理页守恒。 */
static void segment_c(uint64_t base)
{
    for (int it = 0; it < 50; ++it) {
        struct kmem_cache *c = kmem_cache_create("stress_c", 128, 16, 0);
        if (!c) {
            printk(KERN_ERR "[STRESS] C: create failed at %d\n", it);
            break;
        }
        void *objs[16];
        clear_ptrs(objs, 16);

        int n = 0;
        for (; n < 16; ++n) {
            objs[n] = kmem_cache_alloc(c);
            if (!objs[n]) break;
        }
        for (int i = 0; i < n; ++i) kmem_cache_free(c, objs[i]);
        kmem_cache_destroy(c);
    }
    print_delta("C", base, free_pages_now());
}

/* D) 整页路径（> 2048 且 <= PAGE_SIZE 走伙伴系统） */
static void segment_d(uint64_t base)
{
    void *big[64];              /* 512 字节，可放栈 */
    clear_ptrs(big, 64);

    int n = 0;
    for (; n < 64; ++n) {
        big[n] = kmalloc(4096);
        if (!big[n]) break;
    }
    printk("[STRESS] D: allocated %d x 4096B\n", n);
    for (int i = 0; i < n; ++i) kfree(big[i]);
    print_delta("D", base, free_pages_now());
}

static int full_internal(void)
{
    /*
     * ★ 关键：在拍基线之前预热全部 kmalloc 大小类。
     *
     * 若此处缺失，segment_b 首次触及的 7 个冷 cache 会各预留 1 个
     * 缓冲 slab，导致 B 段结束后 free 相对 baseline 下跌 7 页，
     * 被误判为「泄漏」。预热把这次性开销搬到 baseline 之前。
     */
    warmup_all_caches();

    uint64_t base = free_pages_now();
    printk("[STRESS] baseline free=%llu\n", (unsigned long long)base);

    segment_a(base);
    segment_b(base);
    segment_c(base);
    segment_d(base);

    uint64_t now = free_pages_now();
    print_delta("FINAL", base, now);
    if (now != base) {
        printk(KERN_WARN "[STRESS] FULL: final != baseline\n");
        return -1;
    }
    /* 附带的不变量：空闲页不得超过可用总数 */
    if (now > pmm_total_pages()) {
        printk(KERN_ERR "[STRESS] FINAL free %llu > total %llu\n",
               (unsigned long long)now,
               (unsigned long long)pmm_total_pages());
        return -1;
    }
    return 0;
}

static int run_full(void)
{
    printk("[STRESS] level=FULL\n");
    if (full_internal() != 0) return -1;
    if (steady_internal() != 0) return -1;
    printk("[STRESS] FULL OK\n");
    return 0;
}

/* ---------- LONG ---------- */

static int long_round(int seed_idx, uint64_t *out_free, int *out_oom)
{
    /* 512 * 8 = 4096 字节，必须 static 以避免 __chkstk */
    static void *slots[512];
    clear_ptrs(slots, 512);

    *out_oom = 0;

    uint32_t seed = 0xA5A5A5A5u ^ (uint32_t)seed_idx;
    for (int op = 0; op < 50000; ++op) {
        uint32_t r = lcg_next(&seed);
        int idx = (int)(r % 512u);
        if (slots[idx]) {
            kfree(slots[idx]);
            slots[idx] = 0;
        } else {
            size_t sz = (size_t)8u << (r % 9u);
            slots[idx] = kmalloc(sz);
            if (!slots[idx]) *out_oom = 1;
        }
    }
    for (int i = 0; i < 512; ++i)
        if (slots[i]) kfree(slots[i]);

    *out_free = free_pages_now();
    return 0;
}

static int run_long(void)
{
    printk("[STRESS] level=LONG\n");

    /* 1) FULL 基线（内含预热） */
    if (full_internal() != 0) return -1;

    /* 2) 3 轮 50000 次随机操作 */
    uint64_t r[3];
    for (int i = 0; i < 3; ++i) {
        int oom = 0;
        long_round(i, &r[i], &oom);
        printk("[STRESS] long round %d free=%llu%s\n",
               i, (unsigned long long)r[i],
               oom ? " (OOM in round)" : "");
        if (oom) return -1;
    }
    if (r[1] != r[2]) {
        printk(KERN_ERR "[STRESS] NOT steady (long): r1=%llu r2=%llu\n",
               (unsigned long long)r[1],
               (unsigned long long)r[2]);
        return -1;
    }

    printk("[STRESS] LONG OK\n");
    return 0;
}

/* ---------- 入口 ---------- */

int stress_run(int level)
{
    switch (level) {
    case OB_STRESS_NONE:   return 0;
    case OB_STRESS_BASIC:  return run_basic();
    case OB_STRESS_STEADY: return run_steady();
    case OB_STRESS_FULL:   return run_full();
    case OB_STRESS_LONG:   return run_long();
    default:
        printk(KERN_ERR "[STRESS] unknown level %d\n", level);
        return -2;
    }
}