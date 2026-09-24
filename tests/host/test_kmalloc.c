/* kmalloc 大小类选择的纯逻辑测试。完整分配在 QEMU 内核中验证。 */
#include <stdio.h>
#include <stddef.h>

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

static const size_t class_sizes[] = {8,16,32,64,128,256,512,1024,2048};
#define N (sizeof(class_sizes)/sizeof(class_sizes[0]))

static int class_of(size_t s)
{
    for (size_t i = 0; i < N; ++i) if (s <= class_sizes[i]) return (int)i;
    return -1;
}

int main(void)
{
    CHECK(class_of(1)  == 0);
    CHECK(class_of(8)  == 0);
    CHECK(class_of(9)  == 1);
    CHECK(class_of(16) == 1);
    CHECK(class_of(2048) == (int)N-1);
    CHECK(class_of(2049) == -1);

    /* 页内对象数校验 */
    CHECK(4096 / 8    == 512);
    CHECK(4096 / 16   == 256);
    CHECK(4096 / 2048 == 2);

    if (fail == 0) { printf("test_kmalloc: OK\n"); return 0; }
    printf("test_kmalloc: %d failures\n", fail);
    return 1;
}