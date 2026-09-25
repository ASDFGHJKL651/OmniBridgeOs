/*===OmniBridgeOs/kernel/arch/x64/user/sysfs.h===*/
#ifndef OMNIBRIDGE_USER_SYSFS_H
#define OMNIBRIDGE_USER_SYSFS_H

#include "vfs.h"

void sysfs_init(void);
struct vfs_superblock *sysfs_mount(void);

#endif /* OMNIBRIDGE_USER_SYSFS_H */
/*===OmniBridgeOs/kernel/arch/x64/user/sysfs.h 结束===*/