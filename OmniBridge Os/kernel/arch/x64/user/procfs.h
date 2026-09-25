/*===OmniBridgeOs/kernel/arch/x64/user/procfs.h===*/
#ifndef OMNIBRIDGE_USER_PROCFS_H
#define OMNIBRIDGE_USER_PROCFS_H

#include "vfs.h"

void procfs_init(void);
struct vfs_superblock *procfs_mount(void);

#endif /* OMNIBRIDGE_USER_PROCFS_H */
/*===OmniBridgeOs/kernel/arch/x64/user/procfs.h 结束===*/