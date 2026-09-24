/*===OmniBridgeOs/kernel/arch/x64/block/virtio_blk.h===*/
#ifndef OMNIBRIDGE_BLOCK_VIRTIO_BLK_H
#define OMNIBRIDGE_BLOCK_VIRTIO_BLK_H

#include "block.h"

/* 探测并初始化 VirtIO 块设备；成功返回 0。 */
int virtio_blk_init(void);

/* 驱动是否就绪。 */
int virtio_blk_ready(void);

/* 返回关联的块设备指针。 */
struct block_device *virtio_blk_device(void);

#endif /* OMNIBRIDGE_BLOCK_VIRTIO_BLK_H */
/*===OmniBridgeOs/kernel/arch/x64/block/virtio_blk.h 结束===*/