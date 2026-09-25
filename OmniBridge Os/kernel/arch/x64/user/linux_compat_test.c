/*===OmniBridgeOs/kernel/arch/x64/user/linux_compat_test.c===*/
/*
 * Linux 兼容层自检（第 19 步）。
 *
 * 覆盖：
 *   - elf64_detect 返回值语义；
 *   - Linux syscall 映射表非空；
 *   - ob_to_linux_errno 转换；
 *   - /proc、/sys、/dev 挂载状态；
 *   - /dev/null 读返回 0；
 *   - /dev/zero 读返回全 0。
 *
 * 人工必须审查：
 *   - 本测试不调用 task_create / pmm_alloc_pages；
 *   - 不产生任何持久副作用；
 *   - 在 main.c 中于根 fs 挂载后、用户测试驱动前调用。
 */
#include "serial.h"
#include "vfs.h"
#include "task.h"
#include "linux_syscall.h"
#include "elf_loader.h"

extern struct vfs_superblock *procfs_mount(void);
extern struct vfs_superblock *sysfs_mount(void);
extern struct vfs_superblock *devfs_mount(void);

static int g_lx_fail = 0;

static void lx_check(const char *desc, int ok)
{
    if (ok) serial_printf("[LINUX-COMPAT-TEST] OK  : %s\n", desc);
    else { serial_printf("[LINUX-COMPAT-TEST] FAIL: %s\n", desc); g_lx_fail++; }
}

void linux_compat_selftest(void);

void linux_compat_selftest(void)
{
    g_lx_fail = 0;
    serial_printf("[LINUX-COMPAT-TEST] === begin ===\n");

    /* 1) elf64_detect */
    {
        uint8_t empty[4] = {0,0,0,0};
        lx_check("elf64_detect(empty) == 0",
                 elf64_detect(empty, sizeof(empty)) == 0);

        uint8_t bad[64];
        for (int i = 0; i < 64; ++i) bad[i] = 0;
        bad[0] = 0x7F; bad[1] = 'E'; bad[2] = 'L'; bad[3] = 'F';
        lx_check("elf64_detect(bad class) == -1",
                 elf64_detect(bad, sizeof(bad)) == -1);

        /* 一个真正 ELF64 的头部 */
        uint8_t good[64];
        for (int i = 0; i < 64; ++i) good[i] = 0;
        good[0] = 0x7F; good[1] = 'E'; good[2] = 'L'; good[3] = 'F';
        good[4] = 2;    /* ELFCLASS64 */
        good[5] = 1;    /* ELFDATA2LSB */
        /* e_type = ET_EXEC = 2，从偏移 16 起 */
        good[16] = 2; good[17] = 0;
        /* e_machine = 62 = EM_X86_64，从偏移 18 起 */
        good[18] = 62; good[19] = 0;
        /* e_phentsize 从偏移 54 起 = sizeof(elf64_phdr) = 56 */
        good[54] = 56; good[55] = 0;
        lx_check("elf64_detect(good) == 1",
                 elf64_detect(good, sizeof(good)) == 1);
    }

    /* 2) 映射表大小（用 errno 转换间接验证初始化成功） */
    {
        lx_check("ob_to_linux_errno(-1) == LINUX_EPERM",
                 ob_to_linux_errno(-1) == LINUX_EPERM);
        lx_check("ob_to_linux_errno(-2) == LINUX_ENOENT",
                 ob_to_linux_errno(-2) == LINUX_ENOENT);
        lx_check("ob_to_linux_errno(-22) == LINUX_EINVAL",
                 ob_to_linux_errno(-22) == LINUX_EINVAL);
        lx_check("ob_to_linux_errno(0) == 0",
                 ob_to_linux_errno(0) == 0);
    }

    /* 3) 挂载状态 */
    lx_check("vfs_is_mounted(\"/proc\") == 1",
             vfs_is_mounted("/proc") == 1);
    lx_check("vfs_is_mounted(\"/sys\") == 1",
             vfs_is_mounted("/sys") == 1);
    lx_check("vfs_is_mounted(\"/dev\") == 1",
             vfs_is_mounted("/dev") == 1);

    /* 4) /dev/null 读返回 0 */
    {
        struct vfs_file *f = 0;
        int rc = vfs_open("/dev/null", VFS_O_RDONLY, &f);
        lx_check("/dev/null open", rc == 0 && f != 0);
        if (rc == 0 && f) {
            char buf[16];
            int64_t n = vfs_read(f, buf, sizeof(buf));
            lx_check("/dev/null reads 0", n == 0);
            vfs_close(f);
        }
    }

    /* 5) /dev/zero 读返回全 0 */
    {
        struct vfs_file *f = 0;
        int rc = vfs_open("/dev/zero", VFS_O_RDONLY, &f);
        lx_check("/dev/zero open", rc == 0 && f != 0);
        if (rc == 0 && f) {
            char buf[16];
            for (int i = 0; i < 16; ++i) buf[i] = 0xAA;
            int64_t n = vfs_read(f, buf, 16);
            int all_zero = (n == 16);
            for (int i = 0; i < 16 && all_zero; ++i)
                if (buf[i] != 0) all_zero = 0;
            lx_check("/dev/zero fills with 0", all_zero);
            vfs_close(f);
        }
    }

    /* 6) /proc/version 可读 */
    {
        struct vfs_file *f = 0;
        int rc = vfs_open("/proc/version", VFS_O_RDONLY, &f);
        lx_check("/proc/version open", rc == 0 && f != 0);
        if (rc == 0 && f) {
            char buf[128];
            int64_t n = vfs_read(f, buf, sizeof(buf) - 1);
            buf[n > 0 ? n : 0] = '\0';
            lx_check("/proc/version nonempty", n > 0);
            vfs_close(f);
        }
    }

    if (g_lx_fail == 0) serial_printf("[LINUX-COMPAT-TEST] selftest OK\n");
    else serial_printf("[LINUX-COMPAT-TEST] selftest FAILED: %d case(s)\n",
                       g_lx_fail);
    serial_printf("[LINUX-COMPAT-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/user/linux_compat_test.c 结束===*/