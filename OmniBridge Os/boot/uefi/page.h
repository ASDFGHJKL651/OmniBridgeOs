#ifndef OMNIBRIDGE_UEFI_PAGE_H
#define OMNIBRIDGE_UEFI_PAGE_H

#include <stdint.h>

/*
 * 建立 4 级页表，输出 PML4 的物理地址。
 *
 * ★ 关键：本函数**不加载 CR3**。
 *   由调用者在 ExitBootServices 成功返回之后、跳转内核之前手动执行
 *   `mov %cr3`。原因：
 *     - 切换到自定义页表后，OVMF 内部事件/驱动回调仍可能被触发；
 *     - 若这些回调运行在错误的页表下，会触发 #PF。例如 OVMF 内置的
 *       VirtIO 网卡 UEFI 驱动会在 ExitBootServices 清理阶段被回调，
 *       访问 MMIO 时崩溃。
 *     - 必须等 ExitBootServices 完全停止 UEFI 服务后，再切换到内核页表。
 *
 * 页表布局（不变）：
 *   PML4[0]   -> 恒等映射 0..4GB（UEFI 栈/代码/数据 + 内核 LMA 都可访问）
 *   PML4[256] -> DirectMap 0xFFFF800000000000 + phys，覆盖 0..4GB
 *   PML4[511] -> 内核镜像 0xFFFFFFFF80000000 起 1GB，物理基址 = kernel_lma
 *
 * kernel_lma 必须 2MB 对齐。
 *
 * out_pml4_phys 输出 PML4 的物理地址。UEFI 环境为恒等映射，因此该值
 * 等于 PML4 数组的"虚拟地址"。调用者在切 CR3 时直接使用。
 */
void uefi_build_page_tables(uint64_t kernel_lma, void **out_pml4_phys);

#endif