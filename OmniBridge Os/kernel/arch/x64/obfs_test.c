/*===OmniBridgeOs/kernel/arch/x64/obfs_test.c===*/
#include "obfs_test.h"
#include "obfs.h"
#include "vfs.h"
#include "serial.h"

/*
 * OBFS 只读文件系统自检。
 *
 * 构造一个最小内存镜像：
 *   block 0 : superblock
 *   block 1 : block bitmap
 *   block 2 : inode bitmap
 *   block 3 : inode table (16 个 inode)
 *   block 4 : root dir 数据块
 *   block 5 : file 数据块
 * 总大小 6 块 = 24 KB。
 *
 * 目录项格式：struct obfs_dirent (80 字节)。
 */

#define IMG_BLOCKS 6
#define IMG_SIZE   (IMG_BLOCKS * 4096)

static uint8_t g_img[IMG_SIZE] __attribute__((aligned(4096)));

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[OBFS-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[OBFS-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static int mem_eq(const void *a, const void *b, uint64_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint64_t i = 0; i < n; ++i) if (x[i] != y[i]) return 0;
    return 1;
}

/* 把名字写入 obfs_dirent */
static void set_dirent(struct obfs_dirent *de, uint64_t ino,
                       uint16_t type, const char *name)
{
    for (uint64_t i = 0; i < sizeof(*de); ++i)
        ((uint8_t *)de)[i] = 0;
    de->ino = ino;
    de->type = type;
    uint16_t n = 0;
    while (name[n] && n < sizeof(de->name) - 1) {
        de->name[n] = name[n];
        ++n;
    }
    de->name_len = n;
    de->name[n] = '\0';
}

/* 在 inode 表中填充一个 inode */
static void put_inode(uint32_t ino, uint16_t mode, uint64_t size,
                      const uint32_t *direct)
{
    uint64_t off = 3 * 4096 + (uint64_t)ino * OBFS_INODE_SIZE;
    struct obfs_inode *di = (struct obfs_inode *)(g_img + off);
    for (uint64_t i = 0; i < sizeof(*di); ++i)
        ((uint8_t *)di)[i] = 0;
    di->mode = mode;
    di->links = 1;
    di->size = size;
    for (int i = 0; i < 12; ++i) di->direct[i] = direct[i];
}

static void build_image(void)
{
    for (uint64_t i = 0; i < IMG_SIZE; ++i) g_img[i] = 0;

    struct obfs_superblock *sb = (struct obfs_superblock *)g_img;
    sb->magic             = OBFS_MAGIC;
    sb->block_size        = OBFS_BLOCK_SIZE;
    sb->total_blocks      = IMG_BLOCKS;
    sb->inode_count       = 16;
    sb->root_inode        = 0;
    sb->block_bitmap_start = 1;
    sb->inode_bitmap_start = 2;
    sb->inode_table_start  = 3;
    sb->data_block_start   = 4;
    for (int i = 0; i < 32; ++i) sb->ita_public_key[i] = (uint8_t)i;

    /* inode 0：root 目录，1 个数据块 (block 4) */
    uint32_t root_direct[12];
    for (int i = 0; i < 12; ++i) root_direct[i] = 0;
    root_direct[0] = 4;
    put_inode(0, VFS_S_IFDIR | 0x1ED,
              sizeof(struct obfs_dirent) * 3,
              root_direct);

    /* inode 1：文件 hello.txt，1 个数据块 (block 5) */
    uint32_t f_direct[12];
    for (int i = 0; i < 12; ++i) f_direct[i] = 0;
    f_direct[0] = 5;
    put_inode(1, VFS_S_IFREG | 0x1A4, 13, f_direct);

    /* block 4：目录项 . / .. / hello.txt */
    struct obfs_dirent *de = (struct obfs_dirent *)(g_img + 4 * 4096);
    set_dirent(&de[0], 0, VFS_FT_DIR, ".");
    set_dirent(&de[1], 0, VFS_FT_DIR, "..");
    set_dirent(&de[2], 1, VFS_FT_REG, "hello.txt");

    /* block 5：文件内容 */
    const char *content = "hello, obfs!\n";
    uint8_t *data = g_img + 5 * 4096;
    for (int i = 0; content[i]; ++i) data[i] = (uint8_t)content[i];
}

void obfs_test(void)
{
    failures = 0;
    serial_printf("\n[OBFS-TEST] === begin ===\n");

    build_image();

    /* 1) 挂载 */
    struct vfs_superblock *sb = obfs_mount(g_img, IMG_SIZE);
    check("obfs_mount non-NULL", sb != 0);
    if (!sb) goto done;

    /* 2) 根 inode 检查 */
    check("root is dir",
          sb->root &&
          ((sb->root->mode & VFS_S_IFMT) == VFS_S_IFDIR));

    /* 3) 通过 ops 直接 lookup（不通过 vfs 全局挂载） */
    struct vfs_inode *file = 0;
    int rc = sb->ops->lookup(sb->root, "hello.txt", &file);
    check("lookup hello.txt", rc == 0 && file != 0);
    check("file size == 13", file && file->size == 13);
    check("file is reg",
          file && ((file->mode & VFS_S_IFMT) == VFS_S_IFREG));

    /* 4) 通过 vfs_file 读 */
    if (file) {
        struct vfs_file f;
        uint8_t *pf = (uint8_t *)&f;
        for (uint64_t i = 0; i < sizeof(f); ++i) pf[i] = 0;
        f.f_inode = file;
        f.f_flags = VFS_O_RDONLY;

        char buf[32];
        for (int i = 0; i < 32; ++i) buf[i] = 0;
        int64_t n = sb->ops->read(&f, buf, 13);
        check("read hello.txt size", n == 13);
        check("read hello.txt content",
              mem_eq(buf, "hello, obfs!\n", 13));
    }

    /* 5) readdir */
    struct vfs_dirent de;
    int found = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        rc = sb->ops->readdir(sb->root, i, &de);
        if (rc != 0) break;
        if (de.name_len == 9 && mem_eq(de.name, "hello.txt", 9))
            found = 1;
    }
    check("readdir found hello.txt", found);

    /* 6) OBFS 只读：create/mkdir/unlink 返回 -EROFS */
    struct vfs_inode *out_i = 0;
    rc = sb->ops->create(sb->root, "x", VFS_S_IFREG | 0644, &out_i);
    check("create -> -EROFS", rc == OB_EROFS);

    rc = sb->ops->mkdir(sb->root, "y", VFS_S_IFDIR | 0755, &out_i);
    check("mkdir -> -EROFS", rc == OB_EROFS);

    rc = sb->ops->unlink(sb->root, "hello.txt");
    check("unlink -> -EROFS", rc == OB_EROFS);

    /* 7) 卸载 */
    obfs_umount(sb);
    sb = 0;

    /* 8) 非法魔数 */
    g_img[0] = 0xFF;
    g_img[1] = 0xEE;
    struct vfs_superblock *bad = obfs_mount(g_img, IMG_SIZE);
    check("bad magic rejected", bad == 0);
    g_img[0] = (uint8_t)(OBFS_MAGIC & 0xFF);
    g_img[1] = (uint8_t)((OBFS_MAGIC >> 8) & 0xFF);

done:
    if (failures == 0) {
        serial_printf("[OBFS-TEST] selftest OK\n");
    } else {
        serial_printf("[OBFS-TEST] selftest FAILED: %d case(s)\n", failures);
    }
    serial_printf("[OBFS-TEST] === end ===\n\n");
}
/*===OmniBridgeOs/kernel/arch/x64/obfs_test.c 结束===*/