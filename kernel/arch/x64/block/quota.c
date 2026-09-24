/*===OmniBridgeOs/kernel/arch/x64/block/quota.c===*/
#include "quota.h"
#include "obfs_rw.h"
#include "serial.h"

void quota_init(void)
{
    serial_printf("[QUOTA] init: inode-level quota\n");
}

/*
 * 说明（人工必须审查）：
 *   本步 quota_check_write 的入参是 struct obfs_inode*，
 *   它指向 struct obfs_inode_rw 的首字段（base），因此二者地址相同。
 *   通过结构体布局约定，可以安全地反推 rw inode。
 *   为防止误用，我们仍通过宏验证布局一致性。
 */
_Static_assert(sizeof(struct obfs_inode) == 256,
               "obfs_inode must be 256 bytes");
_Static_assert(offsetof(struct obfs_inode_rw, base) == 0,
               "obfs_inode_rw.base must be at offset 0");

int quota_check_write(const struct obfs_inode *ino, uint64_t new_size)
{
    if (!ino) return OB_EINVAL;
    uint64_t limit = obfs_inode_get_quota(ino);
    if (limit == 0) return 0;
    if (new_size > limit) return OB_EDQUOT;
    return 0;
}

int quota_set_inode(struct obfs_inode *ino, uint64_t limit_bytes)
{
    if (!ino) return OB_EINVAL;
    return obfs_inode_set_quota(ino, limit_bytes);
}

uint64_t quota_get_inode(const struct obfs_inode *ino)
{
    if (!ino) return 0;
    return obfs_inode_get_quota(ino);
}
/*===OmniBridgeOs/kernel/arch/x64/block/quota.c 结束===*/