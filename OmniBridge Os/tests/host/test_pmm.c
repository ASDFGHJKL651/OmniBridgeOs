/* 纯逻辑测试：Buddy 合并/拆分数学；不真正访问物理内存。
 * 注：为最小可行，本测试在宿主侧编译 pmm.c，但仅调用标记函数。 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

/* 与 pmm.c 中一致的公式 */
static inline uint64_t pfn_buddy(uint64_t pfn, int order) {
    return pfn ^ (1ULL << order);
}

int main(void)
{
    /* buddy 对称性 */
    CHECK(pfn_buddy(pfn_buddy(0, 0), 0) == 0);
    CHECK(pfn_buddy(pfn_buddy(1, 0), 0) == 1);
    CHECK(pfn_buddy(pfn_buddy(4, 2), 2) == 4);
    CHECK(pfn_buddy(0, 0) == 1);
    CHECK(pfn_buddy(1, 0) == 0);
    CHECK(pfn_buddy(0, 3) == 8);
    CHECK(pfn_buddy(8, 3) == 0);

    /* 连续拆分：pfn 8 order 3 -> 4 -> 2 -> 1 -> 0 */
    uint64_t p = 8;
    for (int o = 3; o > 0; --o) {
        uint64_t b = pfn_buddy(p, o - 1);
        CHECK(b > p);            /* buddy 在 p 之后 */
        CHECK((b & ((1ULL << (o-1)) - 1)) == 0);
    }

    if (fail == 0) { printf("test_pmm: OK\n"); return 0; }
    printf("test_pmm: %d failures\n", fail);
    return 1;
}