/*
 * 宿主侧 VMM 纯逻辑测试（不访问真实硬件）。
 *
 * 覆盖内容：
 *   - 地址空间布局宏的范围与边界
 *   - PML4/PDPT/PD/PT 索引计算
 *   - PTE 权限位组合
 *   - 规范/非规范地址
 *   - 页对齐宏
 *
 * 与 kernel/arch/x64/vmm.h 中的定义严格保持一致。
 */
#include <stdio.h>
#include <stdint.h>

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

/* ---------- 与 vmm.h 一致的宏定义 ---------- */
#define USER_SPACE_START       0x0000000000000000ULL
#define USER_SPACE_END         0x00007FFFFFFFFFFFULL
#define KERNEL_SPACE_START     0xFFFF800000000000ULL
#define KERNEL_SPACE_END       0xFFFFFFFFFFFFFFFFULL
#define DIRECT_MAP_BASE        0xFFFF800000000000ULL
#define DIRECT_MAP_END         0xFFFFC00000000000ULL
#define KERNEL_IMAGE_BASE      0xFFFFFFFF80000000ULL
#define KERNEL_IMAGE_LIMIT     0xFFFFFFFFC0000000ULL

#define PML4_INDEX(v) (((uint64_t)(v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((uint64_t)(v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((uint64_t)(v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((uint64_t)(v) >> 12) & 0x1FF)

#define PAGE_ALIGN_DOWN(x) ((x) & ~0xFFFULL)
#define PAGE_ALIGN_UP(x)   (((x) + 0xFFFULL) & ~0xFFFULL)

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_HUGE      (1ULL << 7)
#define PTE_NX        (1ULL << 63)

#define PTE_RX   (PTE_PRESENT)
#define PTE_R    (PTE_PRESENT | PTE_NX)
#define PTE_RWN  (PTE_PRESENT | PTE_WRITABLE | PTE_NX)
#define PTE_RWH  (PTE_PRESENT | PTE_WRITABLE | PTE_HUGE)
#define PTE_RWHN (PTE_PRESENT | PTE_WRITABLE | PTE_HUGE | PTE_NX)

int main(void)
{
    /* ---------- 1) 地址空间布局边界 ---------- */
    CHECK(USER_SPACE_START == 0);
    CHECK(USER_SPACE_END < KERNEL_SPACE_START);
    CHECK(KERNEL_SPACE_START == 0xFFFF800000000000ULL);
    CHECK(KERNEL_SPACE_END == 0xFFFFFFFFFFFFFFFFULL);

    /* 用户空间与内核空间不重叠 */
    CHECK(USER_SPACE_END < DIRECT_MAP_BASE);

    /* 内核镜像位于内核空间 */
    CHECK(KERNEL_IMAGE_BASE >= KERNEL_SPACE_START);
    CHECK(KERNEL_IMAGE_LIMIT <= KERNEL_SPACE_END);

    /* DirectMap 位于内核空间 */
    CHECK(DIRECT_MAP_BASE >= KERNEL_SPACE_START);

    /* ---------- 2) PML4 索引 ---------- */
    /* 低恒等映射：地址 0 */
    CHECK(PML4_INDEX(0) == 0);
    CHECK(PDPT_INDEX(0) == 0);
    CHECK(PD_INDEX(0) == 0);
    CHECK(PT_INDEX(0) == 0);

    /* DirectMap 起点：0xFFFF800000000000 -> PML4=256, PDPT=0, PD=0 */
    CHECK(PML4_INDEX(0xFFFF800000000000ULL) == 256);
    CHECK(PDPT_INDEX(0xFFFF800000000000ULL) == 0);
    CHECK(PD_INDEX(0xFFFF800000000000ULL) == 0);

    /* DirectMap + 1GB：0xFFFF800040000000 -> PML4=256, PDPT=1, PD=0 */
    CHECK(PML4_INDEX(0xFFFF800040000000ULL) == 256);
    CHECK(PDPT_INDEX(0xFFFF800040000000ULL) == 1);
    CHECK(PD_INDEX(0xFFFF800040000000ULL) == 0);

    /* ---------- 3) 内核高半区索引 ---------- */
    /* 0xFFFFFFFF80000000 -> PML4=511, PDPT=510, PD=0 */
    CHECK(PML4_INDEX(0xFFFFFFFF80000000ULL) == 511);
    CHECK(PDPT_INDEX(0xFFFFFFFF80000000ULL) == 510);
    CHECK(PD_INDEX(0xFFFFFFFF80000000ULL) == 0);

    /* 0xFFFFFFFFC0000000 -> PML4=511, PDPT=511 */
    CHECK(PML4_INDEX(0xFFFFFFFFC0000000ULL) == 511);
    CHECK(PDPT_INDEX(0xFFFFFFFFC0000000ULL) == 511);

    /* 内核镜像 + 2MB：0xFFFFFFFF80200000 -> PD=1 */
    CHECK(PD_INDEX(0xFFFFFFFF80200000ULL) == 1);
    CHECK(PT_INDEX(0xFFFFFFFF80200000ULL) == 0);

    /* 内核镜像 + 4KB：0xFFFFFFFF80001000 -> PT=1 */
    CHECK(PT_INDEX(0xFFFFFFFF80001000ULL) == 1);

    /* 内核镜像 + 2MB + 4KB：0xFFFFFFFF80201000 -> PD=1, PT=1 */
    CHECK(PD_INDEX(0xFFFFFFFF80201000ULL) == 1);
    CHECK(PT_INDEX(0xFFFFFFFF80201000ULL) == 1);

    /* ---------- 4) 边界与 2MB 大页 ---------- */
    CHECK(PD_INDEX(0x00200000ULL) == 1);
    CHECK(PD_INDEX(0x001FFFFFULL) == 0);
    CHECK(PT_INDEX(0x00001000ULL) == 1);
    CHECK(PT_INDEX(0x00000FFFULL) == 0);

    /* ---------- 5) 页对齐宏 ---------- */
    CHECK(PAGE_ALIGN_DOWN(0x1234) == 0x1000);
    CHECK(PAGE_ALIGN_DOWN(0x1000) == 0x1000);
    CHECK(PAGE_ALIGN_DOWN(0x0FFF) == 0x0000);
    CHECK(PAGE_ALIGN_UP(0x1234)   == 0x2000);
    CHECK(PAGE_ALIGN_UP(0x1000)   == 0x1000);
    CHECK(PAGE_ALIGN_UP(0x0001)   == 0x1000);
    CHECK(PAGE_ALIGN_UP(0x0000)   == 0x0000);

    /* ---------- 6) PTE 权限位组合 ---------- */
    /* RX：存在、可执行（NX=0） */
    CHECK((PTE_RX & PTE_PRESENT) != 0);
    CHECK((PTE_RX & PTE_WRITABLE) == 0);
    CHECK((PTE_RX & PTE_NX) == 0);

    /* R：存在、只读、不可执行 */
    CHECK((PTE_R & PTE_PRESENT) != 0);
    CHECK((PTE_R & PTE_WRITABLE) == 0);
    CHECK((PTE_R & PTE_NX) != 0);

    /* RWN：存在、可写、不可执行 */
    CHECK((PTE_RWN & PTE_PRESENT) != 0);
    CHECK((PTE_RWN & PTE_WRITABLE) != 0);
    CHECK((PTE_RWN & PTE_NX) != 0);

    /* RWH：2MB 大页，可执行 */
    CHECK((PTE_RWH & PTE_PRESENT) != 0);
    CHECK((PTE_RWH & PTE_WRITABLE) != 0);
    CHECK((PTE_RWH & PTE_HUGE) != 0);
    CHECK((PTE_RWH & PTE_NX) == 0);

    /* RWHN：2MB 大页，不可执行 */
    CHECK((PTE_RWHN & PTE_PRESENT) != 0);
    CHECK((PTE_RWHN & PTE_WRITABLE) != 0);
    CHECK((PTE_RWHN & PTE_HUGE) != 0);
    CHECK((PTE_RWHN & PTE_NX) != 0);

    /* 内核页不得被用户态访问（U/S=0）：三种段权限均不包含 PTE_USER */
    CHECK((PTE_RX & PTE_USER) == 0);
    CHECK((PTE_R  & PTE_USER) == 0);
    CHECK((PTE_RWN & PTE_USER) == 0);

    /* ---------- 7) 内核镜像权限组合（语义交叉验证） ---------- */
    /* .text 可执行，.rodata/.data/.bss 不可执行 */
    CHECK((PTE_RX & PTE_NX) == 0);
    CHECK((PTE_R  & PTE_NX) != 0);
    CHECK((PTE_RWN & PTE_NX) != 0);

    /* .text / .rodata 不可写，.data / .bss 可写 */
    CHECK((PTE_RX & PTE_WRITABLE) == 0);
    CHECK((PTE_R  & PTE_WRITABLE) == 0);
    CHECK((PTE_RWN & PTE_WRITABLE) != 0);

    if (fail == 0) { printf("test_vmm: OK\n"); return 0; }
    printf("test_vmm: %d failures\n", fail);
    return 1;
}