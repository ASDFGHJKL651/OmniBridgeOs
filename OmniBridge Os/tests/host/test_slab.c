/*
 * SLAB 纯逻辑测试：仅验证大小类布局与对齐数学，不实际访问内存。
 * 与 kernel/arch/x64/slab.c 中的 calc_layout 一致。
 */
#include <stdio.h>
#include <stdint.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1ULL << PAGE_SHIFT)
#define MAX_ORDER  10

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

static size_t round_up(size_t v, size_t a)
{
    if (a == 0) a = 8;
    return (v + a - 1) & ~(a - 1);
}

static int calc_layout(size_t obj_size, size_t *out_objs, size_t *out_order)
{
    size_t min_obj = obj_size;
    if (min_obj < sizeof(void *)) min_obj = sizeof(void *);

    for (int order = 0; order < MAX_ORDER; ++order) {
        size_t bytes = PAGE_SIZE << order;
        size_t nobj  = bytes / min_obj;
        if (nobj >= 2) {
            *out_objs  = nobj;
            *out_order = (size_t)order;
            return 0;
        }
    }
    return -1;
}

int main(void)
{
    size_t nobj, order;

    /* 8 字节对象：4K / 8 = 512，order = 0 */
    CHECK(calc_layout(8, &nobj, &order) == 0);
    CHECK(nobj == 512);
    CHECK(order == 0);

    /* 2048 字节对象：4K / 2048 = 2，order = 0 */
    CHECK(calc_layout(2048, &nobj, &order) == 0);
    CHECK(nobj == 2);
    CHECK(order == 0);

    /* 4096 字节对象：4K / 4096 = 1 < 2，需要 order = 1 -> 8192/4096 = 2 */
    CHECK(calc_layout(4096, &nobj, &order) == 0);
    CHECK(nobj == 2);
    CHECK(order == 1);

    /* 对齐数学 */
    CHECK(round_up(1, 8)    == 8);
    CHECK(round_up(9, 8)    == 16);
    CHECK(round_up(96, 8)   == 96);
    CHECK(round_up(97, 16)  == 112);
    CHECK(round_up(2048, 8) == 2048);

    if (fail == 0) { printf("test_slab: OK\n"); return 0; }
    printf("test_slab: %d failures\n", fail);
    return 1;
}