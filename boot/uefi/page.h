#ifndef OMNIBRIDGE_UEFI_PAGE_H
#define OMNIBRIDGE_UEFI_PAGE_H

#include <stdint.h>

/* 建立并加载 4 级页表：
 *   PML4[0]   -> 恒等映射 0..4GB（过渡用，让 UEFI 栈继续有效）
 *   PML4[256] -> DirectMap 0xFFFF800000000000 + phys，覆盖 0..4GB
 *   PML4[511] -> 内核镜像 0xFFFFFFFF80000000 起 1GB，物理基址 = kernel_lma
 * kernel_lma 必须 2MB 对齐。 */
void uefi_build_page_tables(uint64_t kernel_lma);

#endif