#ifndef OMNIBRIDGE_KMALLOC_H
#define OMNIBRIDGE_KMALLOC_H

#include <stddef.h>

/*
 * 通用内核内存分配接口。
 *
 * 本步起 kmalloc 家族全部构建在 SLAB 分配器之上：
 *   - 小对象（<= 2048 字节）走固定大小类 kmem_cache；
 *   - 大对象（> 2048 且 <= PAGE_SIZE）直接从伙伴系统分配整页；
 *   - kfree 通过页头信息自动判断对象归属。
 *
 * 接口保持向后兼容，调用方无需修改。
 */

void  kmalloc_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);
void *kmalloc_aligned(size_t size, size_t align);

#endif /* OMNIBRIDGE_KMALLOC_H */