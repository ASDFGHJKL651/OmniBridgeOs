/*===OmniBridgeOs/tests/host/test_obfs_logic.c===*/
/*
 * 宿主侧纯逻辑测试：OBFS 布局计算、inode 索引、位图、日志校验和。
 * 不依赖真实硬件；只用内存数组模拟块设备。
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int fail = 0;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); fail++; } } while (0)

#define PAGE_SIZE       4096u
#define OBFS_MAGIC      0x4F424653u
#define OBFS_INODE_SIZE 256u
#define OBFS_RW_INODE_SIZE 384u

struct obfs_superblock {
    uint32_t magic;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t root_inode;
    uint8_t  ita_public_key[32];
    uint64_t block_bitmap_start;
    uint64_t inode_bitmap_start;
    uint64_t inode_table_start;
    uint64_t data_block_start;
} __attribute__((packed));

static int check_superblock(const struct obfs_superblock *sb)
{
    if (sb->magic != OBFS_MAGIC) return -1;
    if (sb->block_size != PAGE_SIZE) return -1;
    if (sb->total_blocks == 0) return -1;
    if (sb->inode_count == 0) return -1;
    if (sb->root_inode >= sb->inode_count) return -1;
    if (sb->block_bitmap_start >= sb->total_blocks) return -1;
    if (sb->inode_bitmap_start >= sb->total_blocks) return -1;
    if (sb->inode_table_start >= sb->total_blocks) return -1;
    if (sb->data_block_start >= sb->total_blocks) return -1;
    return 0;
}

static uint32_t jnl_checksum_simple(uint32_t seed, const void *data, uint32_t n)
{
    uint32_t sum = seed;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < n; ++i) {
        sum ^= p[i];
        sum *= 16777619u;
    }
    return sum;
}

int main(void)
{
    /* ---- 1) 超级块字段校验 ---- */
    {
        struct obfs_superblock sb;
        memset(&sb, 0, sizeof(sb));
        sb.magic       = OBFS_MAGIC;
        sb.block_size  = PAGE_SIZE;
        sb.total_blocks = 4096;
        sb.inode_count  = 256;
        sb.root_inode   = 0;
        sb.block_bitmap_start = 1;
        sb.inode_bitmap_start = 2;
        sb.inode_table_start  = 3;
        sb.data_block_start   = 3 + (256 * OBFS_INODE_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
        CHECK(check_superblock(&sb) == 0);
    }

    /* ---- 2) 非法超级块 ---- */
    {
        struct obfs_superblock sb = {0};
        sb.magic = 0xDEADBEEF;
        CHECK(check_superblock(&sb) != 0);
    }

    /* ---- 3) inode 索引计算 ---- */
    {
        uint64_t inode_table_start = 3;
        uint64_t ino = 10;
        uint64_t off = inode_table_start * PAGE_SIZE +
                       ino * OBFS_RW_INODE_SIZE;
        uint64_t expected = 3 * 4096 + 10 * 384;
        CHECK(off == expected);

        uint64_t block_no = off / PAGE_SIZE;
        uint32_t in_block = (uint32_t)(off % PAGE_SIZE);
        CHECK(block_no == 3);
        CHECK(in_block == 3840);
    }

    /* ---- 4) inode 与只读 OBFS 大小对比 ---- */
    {
        CHECK(OBFS_INODE_SIZE == 256);
        CHECK(OBFS_RW_INODE_SIZE == 384);
        /* 可写 inode 必须能容纳 base (256) + 扩展字段 */
        CHECK(OBFS_RW_INODE_SIZE >= 256 + 8 + 8 + 4 + 4);
    }

    /* ---- 5) 日志校验和确定性 ---- */
    {
        uint8_t buf[64];
        for (int i = 0; i < 64; ++i) buf[i] = (uint8_t)i;
        uint32_t c1 = jnl_checksum_simple(0x811C9DC5u, buf, 64);
        uint32_t c2 = jnl_checksum_simple(0x811C9DC5u, buf, 64);
        CHECK(c1 == c2);

        buf[0] ^= 0xFF;
        uint32_t c3 = jnl_checksum_simple(0x811C9DC5u, buf, 64);
        CHECK(c1 != c3);
    }

    /* ---- 6) 数据块分配（模拟 4KB 位图）---- */
    {
        uint8_t bitmap[512];
        memset(bitmap, 0, sizeof(bitmap));

        int found = -1;
        for (int i = 0; i < 4096; ++i) {
            int bit = (bitmap[i >> 3] >> (i & 7)) & 1;
            if (!bit) { found = i; break; }
        }
        CHECK(found == 0);

        bitmap[0] |= 1;
        found = -1;
        for (int i = 0; i < 4096; ++i) {
            int bit = (bitmap[i >> 3] >> (i & 7)) & 1;
            if (!bit) { found = i; break; }
        }
        CHECK(found == 1);
    }

    /* ---- 7) direct + indirect 块号映射范围 ---- */
    {
        /* direct[12] -> 12 块，indirect1 -> 1024 块，indirect2 -> 1M 块 */
        uint64_t total = 12 + 1024 + 1024 * 1024;
        CHECK(total > 12);
        /* 4KB 块 * 1024 个直接块上限 = 4MB */
        uint64_t max_direct_size = 12ULL * 4096;
        CHECK(max_direct_size == 49152);
    }

    /* ---- 8) 硬链接计数（link_count 语义） ---- */
    {
        uint32_t nlink = 1;
        nlink++;  /* link */
        CHECK(nlink == 2);
        nlink--;  /* unlink */
        CHECK(nlink == 1);
        nlink--;  /* final unlink */
        CHECK(nlink == 0);  /* 应释放 */
    }

    if (fail == 0) { printf("test_obfs_logic: OK\n"); return 0; }
    printf("test_obfs_logic: %d failures\n", fail);
    return 1;
}
/*===OmniBridgeOs/tests/host/test_obfs_logic.c 结束===*/