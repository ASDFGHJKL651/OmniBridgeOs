/*===OmniBridgeOs/kernel/arch/x64/block/quota.h===*/
#ifndef OMNIBRIDGE_BLOCK_QUOTA_H
#define OMNIBRIDGE_BLOCK_QUOTA_H

#include <stdint.h>
#include <stddef.h>     /* ★ offsetof */

#ifndef OB_EDQUOT
#define OB_EDQUOT (-122)
#endif

struct obfs_inode;

void quota_init(void);

int quota_check_write(const struct obfs_inode *ino, uint64_t new_size);
int quota_set_inode(struct obfs_inode *ino, uint64_t limit_bytes);
uint64_t quota_get_inode(const struct obfs_inode *ino);

#endif /* OMNIBRIDGE_BLOCK_QUOTA_H */
/*===OmniBridgeOs/kernel/arch/x64/block/quota.h 结束===*/