/*===OmniBridgeOs/kernel/arch/x64/mem_domain.h===*/
#ifndef OMNIBRIDGE_MEM_DOMAIN_H
#define OMNIBRIDGE_MEM_DOMAIN_H

#include <stdint.h>
#include "task.h"

/*
 * 权限 0/1 进程的内存域（第 15 步）。
 *
 * 关键不变量（人工必须审查）：
 *   1) 每个权限 0/1 进程拥有独立的 PML4；PML4[0..255] 用户空间部分
 *      不与任何其他进程共享；PML4[256]/PML4[511] 为内核高半区，
 *      与内核主 PML4 共享（见 vmm_create_address_space）。
 *   2) 物理页帧一律来自 pmm_alloc_pages(0)，绝不来自 kmalloc/SLAB。
 *   3) 元数据数组（struct page * 数组）由 kmalloc 分配——这是内核
 *      私有数据，不是进程可访问的内存。
 *   4) 所有字段的访问顺序为：先 destroy，再重建（create 拒绝已就绪）。
 */

/* 权限 0/1 的物理页帧上限（编译期常量） */
#define MEM_DOMAIN_PRIV0_MAX_PAGES   16ULL   /* 64 KiB */
#define MEM_DOMAIN_PRIV1_MAX_PAGES   64ULL   /* 256 KiB */

/* 创建时预分配页数 */
#define MEM_DOMAIN_PRIV0_INIT_PAGES  4ULL
#define MEM_DOMAIN_PRIV1_INIT_PAGES  8ULL

/* 地址范围：base 含，limit 不含 */
#define MEM_DOMAIN_PRIV0_BASE_VADDR  0x0000000000400000ULL  /* 4 MiB */
#define MEM_DOMAIN_PRIV0_LIMIT_VADDR 0x0000000000800000ULL  /* 8 MiB */
#define MEM_DOMAIN_PRIV1_BASE_VADDR  0x0000000000400000ULL  /* 4 MiB */
#define MEM_DOMAIN_PRIV1_LIMIT_VADDR 0x0000000001000000ULL  /* 16 MiB */

/* 元数据数组容量（struct page * 数组，每项 8 字节 = 512 字节） */
#define MEM_DOMAIN_MAX_PAGES         64

/* 创建内存域：分配独立 PML4、建立用户空间页表、预分配物理页帧。
 * 返回 0 成功；负错误码失败（此时所有已分配资源已回滚）。 */
int mem_domain_create(struct task_t *t);

/* 销毁内存域：释放所有独占物理页帧、递归释放用户空间页表、释放 PML4。
 * 允许 t == NULL 或 t->mem_domain.pml4_self_ptr == NULL（无操作）。 */
void mem_domain_destroy(struct task_t *t);

/* 在域内映射一个 4KB 页。vaddr 必须落在 [base, limit)。
 * flags 会自动叠加 PTE_USER。 */
int mem_domain_map_page(struct task_t *t, uint64_t vaddr,
                        uint64_t phys, uint64_t flags);

/* vaddr 是否在当前域内（含 base，不含 limit）。 */
int mem_domain_contains(const struct task_t *t, uint64_t vaddr);

/* 从伙伴系统申请 n 个独占物理页帧并映射到域内的连续虚拟地址。
 * 成功返回 0；若超出 quota 或 PMM 耗尽，自动回滚已成功的部分，
 * 返回 OB_ENOMEM。 */
int mem_domain_grow(struct task_t *t, uint64_t n);

/* 当前域内已分配的物理页帧数。 */
uint64_t mem_domain_used_pages(const struct task_t *t);

#endif /* OMNIBRIDGE_MEM_DOMAIN_H */
/*===OmniBridgeOs/kernel/arch/x64/mem_domain.h 结束===*/