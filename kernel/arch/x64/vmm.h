/*===OmniBridgeOs/kernel/arch/x64/vmm.h===*/
#ifndef OMNIBRIDGE_VMM_H
#define OMNIBRIDGE_VMM_H

#include <stdint.h>

/* ============================================================
 * 地址空间布局（人工必须审查）
 *
 * 用户空间： 0x0000000000000000 ~ 0x00007FFFFFFFFFFF
 * 内核空间： 0xFFFF800000000000 ~ 0xFFFFFFFFFFFFFFFF
 * DirectMap：0xFFFF800000000000 + phys （覆盖低 4GB）
 * 内核镜像： 0xFFFFFFFF80000000 起 1GB（物理基址 OB_KERNEL_LMA）
 * ============================================================ */
#define USER_SPACE_START      0x0000000000000000ULL
#define USER_SPACE_END        0x00007FFFFFFFFFFFULL
#define KERNEL_SPACE_START    0xFFFF800000000000ULL
#define KERNEL_SPACE_END      0xFFFFFFFFFFFFFFFFULL
#define DIRECTMAP_BASE        0xFFFF800000000000ULL
#define KERNEL_VMA_BASE       0xFFFFFFFF80000000ULL

/* 页表索引宏 */
#define PML4_INDEX(v) (((uint64_t)(v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((uint64_t)(v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((uint64_t)(v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((uint64_t)(v) >> 12) & 0x1FF)

/* 页表项标志 */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_WRITETHRU (1ULL << 3)
#define PTE_NOCACHE   (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* 初始化内核页表：建立低恒等映射、DirectMap、内核高半区映射，并加载 CR3 */
void vmm_init(void);

/* 当前 CR3 指向的 PML4（虚拟地址） */
uint64_t *vmm_current_pml4(void);

/* 在给定 PML4 中映射一个 2MB 大页（向后兼容接口） */
int vmm_map_2mb(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/* 在给定 PML4 中映射一个 4KB 页，自动创建中间页表 */
int vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/* 取消映射一个 4KB 页 */
int vmm_unmap_page(uint64_t *pml4, uint64_t virt);

/* 获取虚拟地址对应的 PTE 指针，若不存在返回 NULL */
uint64_t *vmm_get_pte(uint64_t *pml4, uint64_t virt);

/* 创建新的地址空间（分配 PML4 并复制内核共享映射：PML4[0]/[256]/[511]） */
uint64_t *vmm_create_address_space(void);

/* 销毁地址空间（释放 PML4 及用户页表） */
void vmm_destroy_address_space(uint64_t *pml4);

/* 切换地址空间（加载 CR3） */
void vmm_switch_address_space(uint64_t *pml4);

/* 刷新 TLB（重新加载 CR3） */
void vmm_flush_tlb(void);

/* 将源 PML4 的内核高半区映射复制到目标 PML4 */
void vmm_copy_kernel_mappings(uint64_t *dst_pml4, uint64_t *src_pml4);

/* 启用 SMEP/SMAP（若 CPU 支持），并打印状态 */
void vmm_enable_smep_smap(void);

/* 查询 SMEP/SMAP 是否已启用 */
int vmm_smep_enabled(void);
int vmm_smap_enabled(void);

/* ============================================================
 * 第 15 步新增
 * ============================================================ */

/* 获取内核主 PML4（vmm_init 时保存）。 */
uint64_t *vmm_kernel_pml4(void);

/* 在给定 PML4 上为用户空间 [base, limit) 建立中间页表（PDPT/PD）。 */
int vmm_setup_user_space(uint64_t *pml4, uint64_t base, uint64_t limit);

/* 释放 PML4 中用户空间部分（PML4[0..255]）的所有页表页。 */
void vmm_free_user_pagetables(uint64_t *pml4);

/* 临时允许/禁止内核访问用户内存（SMAP 开启后使用） */
static inline void stac(void) { __asm__ __volatile__("stac" ::: "memory"); }
static inline void clac(void) { __asm__ __volatile__("clac" ::: "memory"); }
int vmm_is_pt_page(uint64_t pa);
#endif /* OMNIBRIDGE_VMM_H */