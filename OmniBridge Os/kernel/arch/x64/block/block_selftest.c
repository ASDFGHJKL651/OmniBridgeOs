/*===OmniBridgeOs/kernel/arch/x64/block/block_selftest.c===*/
#include "block_selftest.h"
#include "block.h"
#include "page_cache.h"
#include "virtio_blk.h"
#include "journal.h"
#include "quota.h"
#include "xattr.h"
#include "symlink.h"
#include "crypto.h"
#include "obfs_rw.h"
#include "mkfs.h"
#include "fsck.h"
#include "serial.h"
#include "kmalloc.h"
#include "pmm.h"

static int g_fail = 0;
static int g_ran_full = 0;

static void check(const char *desc, int ok)
{
    if (ok) serial_printf("[BLOCK-TEST] OK  : %s\n", desc);
    else { serial_printf("[BLOCK-TEST] FAIL: %s\n", desc); g_fail++; }
}

/* ============================================================
 * 基础单元测试
 * ============================================================ */
static void test_dma(void)
{
    void *p = block_dma_alloc(1);
    check("block_dma_alloc(1) non-NULL", p != 0);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        b[0] = 0xAA; b[4095] = 0x55;
        check("dma write/read pattern",
              b[0] == 0xAA && b[4095] == 0x55);
        block_dma_free(p, 1);
    }
    void *q = block_dma_alloc(2);
    check("block_dma_alloc(2) non-NULL", q != 0);
    if (q) block_dma_free(q, 2);
}

static void test_devices(void)
{
    serial_printf("[BLOCK-TEST] device count=%u\n",
                  (unsigned)block_device_count());
    check("block device count consistent",
          block_device_count() > 0 || virtio_blk_ready() == 0);
}

static void test_pcache(void)
{
    pcache_init();
    check("pcache_init done", 1);

    struct block_device *dev = virtio_blk_device();
    if (!dev) {
        serial_printf("[BLOCK-TEST] NOTE: no virtio-blk, skip pcache IO\n");
        return;
    }
    uint8_t *b = 0;
    int rc = pcache_get(dev, 0, &b, 0);
    check("pcache_get block 0", rc == 0 && b != 0);
}

static void test_journal(void)
{
    journal_init();
    check("journal_init done", 1);
    check("journal crash stage default 0",
          journal_get_crash_stage() == 0);
}

static void test_quota(void)
{
    quota_init();
    check("quota_init done", 1);
}

static void test_xattr(void)
{
    xattr_init();

    struct xattr_set xs;
    for (size_t i = 0; i < sizeof(xs); ++i) ((uint8_t *)&xs)[i] = 0;

    int rc = xattr_set(&xs, "user.test", "hello", 5);
    check("xattr set", rc == 0);
    char buf[16];
    uint16_t blen = sizeof(buf);
    rc = xattr_get(&xs, "user.test", buf, &blen);
    check("xattr get",
          rc == 0 && blen == 5 && buf[0] == 'h' && buf[4] == 'o');
    rc = xattr_remove(&xs, "user.test");
    check("xattr remove", rc == 0);
    blen = sizeof(buf);
    rc = xattr_get(&xs, "user.test", buf, &blen);
    check("xattr after remove -ENODATA", rc == -61);
}

static void test_symlink(void)
{
    symlink_init();
    check("symlink /kernel blocked",
          symlink_check_target("/kernel/x") != 0);
    check("symlink /system/critical blocked",
          symlink_check_target("/system/critical/x") != 0);
    check("symlink normal path allowed",
          symlink_check_target("/tmp/foo") == 0);

    char out[256];
    int rc = symlink_resolve("target", "/home", out, sizeof(out));
    check("symlink resolve relative",
          rc == 0 && out[1] == 'h' && out[6] == 't');
}

static void test_crypto(void)
{
    crypto_init();
    check("crypto init done", 1);
    check("crypto algo NONE", crypto_get_algo() == 0);
    int rc = crypto_set_key(1, 0, 0);
    check("crypto_set_key(AES) -ENOSYS", rc == -38);
}

/* ============================================================
 * OBFS-RW 端到端
 * ============================================================ */
static void test_obfs_rw_full(void)
{
    struct block_device *dev = virtio_blk_device();
    if (!dev) {
        serial_printf("[BLOCK-TEST] NOTE: no virtio-blk, skip obfs_rw\n");
        return;
    }
    if (dev->total_sectors < 2048) {
        serial_printf("[BLOCK-TEST] NOTE: disk too small\n");
        return;
    }

    g_ran_full = 1;
    uint64_t total_blocks = (dev->total_sectors * 512) / PAGE_SIZE;

    /* ---- 1) 若未格式化则格式化 ---- */
    if (!obfs_is_formatted(dev, 0)) {
        serial_printf("[BLOCK-TEST] image not formatted, running mkfs\n");
        int rc = obfs_format(dev, 0, total_blocks, 256);
        check("mkfs format", rc == 0);
        if (rc != 0) return;
    } else {
        serial_printf("[BLOCK-TEST] image already formatted\n");
    }

    /* ---- 2) 挂载 ---- */
    struct vfs_superblock *sb = obfs_rw_mount(dev, 0, total_blocks);
    check("obfs_rw_mount", sb != 0);
    if (!sb) return;
    check("obfs_rw writable", obfs_rw_is_writable(sb) == 1);

    /* ---- 3) 创建 + 写 + 读 ---- */
    struct vfs_inode *file = 0;
    int rc = sb->ops->create(sb->root, "t1.txt",
                             VFS_S_IFREG | 0644, &file);
    check("create t1.txt", rc == 0 && file != 0);
    if (rc == 0 && file) {
        struct vfs_file f;
        uint8_t *pf = (uint8_t *)&f;
        for (size_t i = 0; i < sizeof(f); ++i) pf[i] = 0;
        f.f_inode = file;

        const char *msg = "hello obfs-rw";
        int64_t nw = sb->ops->write(&f, msg, 14);
        check("write 14 bytes", nw == 14);

        f.f_pos = 0;
        char rbuf[32];
        int64_t nr = sb->ops->read(&f, rbuf, 14);
        int ok = (nr == 14);
        if (ok) {
            for (int i = 0; i < 14; ++i)
                if (rbuf[i] != msg[i]) { ok = 0; break; }
        }
        check("read back matches", ok);
    }

    /* ---- 4) 大文件（>48KB）走 indirect ---- */
    struct vfs_inode *big = 0;
    rc = sb->ops->create(sb->root, "big.bin",
                         VFS_S_IFREG | 0644, &big);
    check("create big.bin", rc == 0 && big != 0);
    if (rc == 0 && big) {
        struct vfs_file f;
        uint8_t *pf = (uint8_t *)&f;
        for (size_t i = 0; i < sizeof(f); ++i) pf[i] = 0;
        f.f_inode = big;

        static uint8_t wbuf[64 * 1024];
        for (int i = 0; i < (int)sizeof(wbuf); ++i)
            wbuf[i] = (uint8_t)(i & 0xFF);

        int64_t nw = sb->ops->write(&f, wbuf, sizeof(wbuf));
        check("write 64KB (indirect1)", nw == (int64_t)sizeof(wbuf));

        f.f_pos = 60 * 1024;
        uint8_t rbuf[512];
        int64_t nr = sb->ops->read(&f, rbuf, 512);
        int ok = (nr == 512);
        if (ok) {
            for (int i = 0; i < 512; ++i) {
                uint8_t expect = (uint8_t)((60 * 1024 + i) & 0xFF);
                if (rbuf[i] != expect) { ok = 0; break; }
            }
        }
        check("read back 60KB offset", ok);
    }

    /* ---- 5) 目录操作 ---- */
    struct vfs_inode *subdir = 0;
    rc = sb->ops->mkdir(sb->root, "sub",
                        VFS_S_IFDIR | 0755, &subdir);
    check("mkdir sub", rc == 0 && subdir != 0);

    /* ---- 6) 硬链接 ---- */
    struct vfs_inode *src = 0;
    rc = sb->ops->lookup(sb->root, "t1.txt", &src);
    check("lookup t1.txt", rc == 0 && src != 0);
    if (rc == 0 && src) {
        rc = obfs_rw_link(sb->root, "t1.link", src);
        check("hardlink t1.link", rc == 0);

        struct vfs_inode *link2 = 0;
        rc = sb->ops->lookup(sb->root, "t1.link", &link2);
        check("lookup t1.link", rc == 0 && link2 != 0);
        check("link shares inode",
              link2 && src && link2->ino == src->ino);
    }

    /* ---- 7) 符号链接 ---- */
    rc = obfs_rw_symlink(sb->root, "sym", "/tmp/target");
    check("symlink /tmp/target", rc == 0);
    {
        struct vfs_inode *sl = 0;
        rc = sb->ops->lookup(sb->root, "sym", &sl);
        check("lookup sym", rc == 0 && sl != 0);
        if (rc == 0 && sl) {
            char buf[64];
            rc = obfs_rw_readlink(sl, buf, sizeof(buf));
            int ok = (rc == 0);
            if (ok) {
                const char *want = "/tmp/target";
                for (int i = 0; want[i]; ++i)
                    if (buf[i] != want[i]) { ok = 0; break; }
            }
            check("readlink sym", ok);
        }
        rc = obfs_rw_symlink(sb->root, "bad", "/kernel/x");
        check("symlink /kernel rejected", rc != 0);
    }

    /* ---- 8) 配额：设置 100 字节限额后写入超限 ---- */
    if (file) {
        struct obfs_inode_rw *rw = obfs_rw_disk_inode(file);
        if (rw) rw->quota_limit = 100;

        struct vfs_file f;
        uint8_t *pf = (uint8_t *)&f;
        for (size_t i = 0; i < sizeof(f); ++i) pf[i] = 0;
        f.f_inode = file;
        f.f_pos   = 200;

        uint8_t big[256];
        for (int i = 0; i < 256; ++i) big[i] = 0;
        int64_t nw = sb->ops->write(&f, big, 256);
        check("quota -EDQUOT on 200+256>100", nw == -122);

        if (rw) rw->quota_limit = 0;
    }

    /* ---- 9) pcache_sync ---- */
    rc = pcache_sync();
    check("pcache_sync returns 0", rc == 0);

    /* ---- 10) fsck 交叉校验 ---- */
    rc = fsck_check(dev, 0, total_blocks);
    check("fsck clean after ops", rc == 0);

    /* ---- 11) 卸载 ---- */
    obfs_rw_umount(sb);
    check("obfs_rw_umount done", 1);

    /* ---- 12) 再次挂载验证持久化 ---- */
    sb = obfs_rw_mount(dev, 0, total_blocks);
    check("re-mount after umount", sb != 0);
    if (sb) {
        struct vfs_inode *persist = 0;
        rc = sb->ops->lookup(sb->root, "t1.txt", &persist);
        check("t1.txt persisted", rc == 0 && persist != 0);

        struct vfs_inode *big2 = 0;
        rc = sb->ops->lookup(sb->root, "big.bin", &big2);
        check("big.bin persisted", rc == 0 && big2 != 0);
        if (big2) {
            check("big.bin size 64KB",
                  big2->size == 64 * 1024);
        }

        obfs_rw_umount(sb);
    }
}

/* ============================================================
 * 崩溃一致性
 * ============================================================ */
static void test_crash_recovery(void)
{
    struct block_device *dev = virtio_blk_device();
    if (!dev) return;
    if (!obfs_is_formatted(dev, 0)) return;

    uint64_t total_blocks = (dev->total_sectors * 512) / PAGE_SIZE;
    struct vfs_superblock *sb = obfs_rw_mount(dev, 0, total_blocks);
    if (!sb) return;

    struct journal *j = obfs_rw_journal(sb);
    check("obfs_rw_journal handle non-NULL", j != 0);

    journal_set_crash_stage(0);
    check("crash stage reset to 0",
          journal_get_crash_stage() == 0);

    int rc = journal_recover(j);
    check("journal_recover returns 0", rc == 0);

    obfs_rw_umount(sb);
}

/* ============================================================
 * 入口
 * ============================================================ */
void block_selftest(void)
{
    g_fail = 0;
    g_ran_full = 0;
    serial_printf("[BLOCK-TEST] === begin ===\n");

    test_dma();
    test_devices();
    test_pcache();
    test_journal();
    test_quota();
    test_xattr();
    test_symlink();
    test_crypto();

    test_obfs_rw_full();
    if (g_ran_full) {
        test_crash_recovery();
    }

    if (g_fail == 0) serial_printf("[BLOCK] selftest OK\n");
    else serial_printf("[BLOCK] selftest FAILED: %d case(s)\n", g_fail);
    serial_printf("[BLOCK-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/block/block_selftest.c 结束===*/