#include <stdio.h>
#include <stdint.h>

/* 与 vmm.h 保持一致 */
#define PML4_INDEX(v) (((uint64_t)(v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((uint64_t)(v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((uint64_t)(v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((uint64_t)(v) >> 12) & 0x1FF)

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

int main(void)
{
    /* 低恒等映射：地址 0 */
    CHECK(PML4_INDEX(0) == 0);
    CHECK(PDPT_INDEX(0) == 0);
    CHECK(PD_INDEX(0) == 0);

    /* DirectMap 起点：0xFFFF800000000000 -> PML4=256, PDPT=0, PD=0 */
    CHECK(PML4_INDEX(0xFFFF800000000000ULL) == 256);
    CHECK(PDPT_INDEX(0xFFFF800000000000ULL) == 0);
    CHECK(PD_INDEX(0xFFFF800000000000ULL) == 0);

    /* DirectMap + 1GB：0xFFFF800040000000 -> PML4=256, PDPT=1, PD=0 */
    CHECK(PML4_INDEX(0xFFFF800040000000ULL) == 256);
    CHECK(PDPT_INDEX(0xFFFF800040000000ULL) == 1);
    CHECK(PD_INDEX(0xFFFF800040000000ULL) == 0);

    /* 内核高半区：0xFFFFFFFF80000000 -> PML4=511, PDPT=510, PD=0 */
    CHECK(PML4_INDEX(0xFFFFFFFF80000000ULL) == 511);
    CHECK(PDPT_INDEX(0xFFFFFFFF80000000ULL) == 510);
    CHECK(PD_INDEX(0xFFFFFFFF80000000ULL) == 0);

    /* 内核高半区 + 1GB：0xFFFFFFFFC0000000 -> PML4=511, PDPT=511, PD=0 */
    CHECK(PML4_INDEX(0xFFFFFFFFC0000000ULL) == 511);
    CHECK(PDPT_INDEX(0xFFFFFFFFC0000000ULL) == 511);

    /* 2MB 边界：0x00200000 -> PD=1 */
    CHECK(PD_INDEX(0x00200000ULL) == 1);
    CHECK(PD_INDEX(0x001FFFFFULL) == 0);

    /* 4KB 边界：0x1000 -> PT=1 */
    CHECK(PT_INDEX(0x00001000ULL) == 1);
    CHECK(PT_INDEX(0x00000FFFULL) == 0);

    if (fail == 0) { printf("test_vmm: OK\n"); return 0; }
    printf("test_vmm: %d failures\n", fail);
    return 1;
}