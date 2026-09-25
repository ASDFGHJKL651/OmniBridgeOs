/*===OmniBridgeOs/kernel/arch/x64/user/devfs.h===*/
#ifndef OMNIBRIDGE_USER_DEVFS_H
#define OMNIBRIDGE_USER_DEVFS_H

#include "vfs.h"

void devfs_init(void);
struct vfs_superblock *devfs_mount(void);

#endif /* OMNIBRIDGE_USER_DEVFS_H */
/*===OmniBridgeOs/kernel/arch/x64/user/devfs.h 结束===*/