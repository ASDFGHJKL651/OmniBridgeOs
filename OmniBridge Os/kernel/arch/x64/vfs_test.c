/*===OmniBridgeOs/kernel/arch/x64/vfs_test.c===*/
#include "vfs_test.h"
#include "vfs.h"
#include "tmpfs.h"
#include "serial.h"

/*
 * 第 11 步 VFS 自检。
 *
 * 设计要点（人工必须审查）：
 *   - 全流程使用一个独立的 tmpfs 实例（max = 1 MiB），测试结束后
 *     通过 vfs_umount 完全释放，不污染系统根文件系统。
 *   - 所有 name 比较一律使用「name_len + 逐字节」方式，**不带前导
 *     斜杠**：vfs_dirent.name 只包含单个路径分量（例如 "file.txt"
 *     而不是 "/file.txt"）。
 *   - 不调用 task_create / pmm_alloc_pages；tmpfs 内部使用 kmalloc，
 *     由 vfs_umount → ops->destroy_sb 全量回收。
 *   - 打印格式与脚本 scripts/check-serial.ps1 保持一致。
 */

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[VFS-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[VFS-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

/* 逐字节比较 de.name 与期望（不含前导 '/'）。
 * 期望字符串以 '\0' 结尾；de.name 由 FS 层保证以 '\0' 结尾。 */
static int name_eq(const struct vfs_dirent *de, const char *expect)
{
    uint32_t i = 0;
    for (;;) {
        char a = de->name[i];
        char b = expect[i];
        if (a != b) return 0;
        if (a == '\0') return 1;
        ++i;
        if (i >= VFS_NAME_MAX) return 0;
    }
}

void vfs_test(void)
{
    failures = 0;
    serial_printf("[VFS-TEST] === begin ===\n");

    /* ---------- 1) 初始化 ---------- */
    vfs_init();
    check("vfs_init: mounts == 0", vfs_mount_count() == 0);

    /* ---------- 2) 挂载 tmpfs ---------- */
    struct vfs_superblock *sb = tmpfs_mount("test-root", 1024 * 1024);
    check("tmpfs_mount non-NULL", sb != 0);
    if (!sb) goto out;

    int rc = vfs_mount("/", sb);
    check("vfs_mount('/')", rc == 0);
    check("mount_count == 1", vfs_mount_count() == 1);

    /* ---------- 3) lookup '/' ---------- */
    struct vfs_inode *root = 0;
    rc = vfs_lookup("/", &root);
    check("lookup('/')", rc == 0 && root != 0);
    if (root) {
        check("root is dir", VFS_S_ISDIR(root->mode));
    }

    /* ---------- 4) mkdir /dir ---------- */
    rc = vfs_mkdir("/dir", 0755);
    check("mkdir /dir", rc == 0);

    struct vfs_inode *dir_ino = 0;
    rc = vfs_lookup("/dir", &dir_ino);
    check("lookup /dir", rc == 0 && dir_ino != 0);

    /* ---------- 5) open 不存在 ---------- */
    struct vfs_file *f = 0;
    rc = vfs_open("/noexist", VFS_O_RDONLY, &f);
    check("open /noexist returns -ENOENT", rc == OB_ENOENT);

    /* ---------- 6) 创建 /file.txt 并写入 "hello" ---------- */
    rc = vfs_open("/file.txt", VFS_O_CREAT | VFS_O_RDWR, &f);
    check("create /file.txt", rc == 0 && f != 0);
    if (rc == 0 && f) {
        const char *payload = "hello";
        int64_t nw = vfs_write(f, payload, 5);
        check("write /file.txt", nw == 5);
        vfs_close(f);
    }

    /* ---------- 7) lookup /file.txt ---------- */
    struct vfs_inode *file_ino = 0;
    rc = vfs_lookup("/file.txt", &file_ino);
    check("lookup /file.txt", rc == 0 && file_ino != 0);

    /* ---------- 8) 打开并读取 ---------- */
    f = 0;
    rc = vfs_open("/file.txt", VFS_O_RDONLY, &f);
    check("open /file.txt", rc == 0 && f != 0);
    if (rc == 0 && f) {
        char rbuf[16];
        int64_t nr = vfs_read(f, rbuf, 5);
        check("read size", nr == 5);
        if (nr == 5) {
            int ok = 1;
            const char *exp = "hello";
            for (int i = 0; i < 5; ++i) if (rbuf[i] != exp[i]) ok = 0;
            check("read content", ok);
        }
        vfs_close(f);
    }

    /* ---------- 9) lookup /dir/../file.txt ---------- */
    struct vfs_inode *via_dotdot = 0;
    rc = vfs_lookup("/dir/../file.txt", &via_dotdot);
    check("lookup /dir/../file.txt", rc == 0 && via_dotdot != 0);

    /* ---------- 10) 内核前缀保护 ---------- */
    struct vfs_inode *kp = 0;
    rc = vfs_lookup("/kernel/foo", &kp);
    check("/kernel/foo rejected", rc == OB_EPERM);

    rc = vfs_lookup("/system/kernel/bar", &kp);
    check("/system/kernel/bar rejected", rc == OB_EPERM);

    /* ---------- 11) readdir root: 完整遍历 ----------
     *
     * 注意（人工必须审查）：
     *   - de.name 只包含单个路径分量，"file.txt" 而不是 "/file.txt"。
     *   - 遍历 index = 0, 1, 2, ... 直到 FS 返回非 0；只要有一次匹配
     *     就视为找到。
     *   - 不假设遍历顺序，也不假设 inode 号。 */
    int found_dir  = 0;
    int found_file = 0;
    int iter_limit = 64;    /* 上限，防 FS 死循环 */

    for (int i = 0; i < iter_limit; ++i) {
        struct vfs_dirent de;
        rc = vfs_readdir(root, (uint64_t)i, &de);
        if (rc != 0) break;

        if (!found_file && name_eq(&de, "file.txt")) found_file = 1;
        if (!found_dir  && name_eq(&de, "dir"))      found_dir  = 1;

        if (found_dir && found_file) break;
    }

    check("readdir root: found /file.txt", found_file);
    check("readdir root: found /dir",      found_dir);

    /* ---------- 12) unlink /file.txt ---------- */
    rc = vfs_unlink("/file.txt");
    check("unlink /file.txt", rc == 0);

    struct vfs_inode *gone = 0;
    rc = vfs_lookup("/file.txt", &gone);
    check("post-unlink lookup fails", rc != 0);

    /* ---------- 13) rmdir /dir ---------- */
    rc = vfs_rmdir("/dir");
    check("rmdir /dir", rc == 0);

    /* ---------- 14) umount ---------- */
    rc = vfs_umount("/");
    check("vfs_umount('/')", rc == 0);
    check("mount_count == 0", vfs_mount_count() == 0);

out:
    if (failures == 0) {
        serial_printf("[VFS-TEST] selftest OK\n");
    } else {
        serial_printf("[VFS-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[VFS-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/vfs_test.c 结束===*/