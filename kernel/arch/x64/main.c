/*===OmniBridgeOs/kernel/arch/x64/main.c===*/
#include "selftest.h"
#include "stress.h"
#include "permission_test.h"
#include "task_test.h"

#include "boot.h"
#include "gdt.h"
#include "tss.h"
#include "idt.h"
#include "pic.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "slab.h"
#include "spinlock.h"
#include "percpu.h"
#include "printk.h"
#include "serial.h"
#include "lapic.h"
#include "ioapic.h"
#include "ipi.h"
#include "sched.h"
#include "task.h"
#include "pid.h"

#include "audit.h"
#include "see.h"
#include "ita.h"
#include "sha384_test.h"
#include "audit_test.h"
#include "see_test.h"
#include "obr_test.h"
#include "ita_test.h"

#include "vfs.h"
#include "tmpfs.h"
#include "vfs_test.h"
#include "obfs_test.h"

#include "critical.h"
#include "critical_test.h"
#include "oshell.h"
#include "boot_services.h"
#include "obinit.h"
#include "obinit_test.h"

#include "uel_test.h"
#include "oshell_test.h"

#include "rng.h"
#include "ita_sign.h"
#include "sha512_test.h"
#include "ed25519_test.h"
#include "ita_sign_test.h"

#include "mem_domain.h"
#include "mem_domain_test.h"
#include "priv_iso.h"
#include "priv_iso_test.h"

#include "see_policy_test.h"
#include "sandbox_test.h"

#include "ita_manual_test.h"
#include "art_test.h"
#include "compat_preload_test.h"

/* ★ 第 18 步 */
#include "sha256_test.h"
#include "compat_env_test.h"
#include "compat_path_test.h"
#include "compat_handle_test.h"
#include "compat_object_test.h"
#include "compat_exception_test.h"

/* ★ 第 18A 步：网络 */
#include "net/net.h"
#include "net/netns.h"
#include "net/net_selftest.h"

/* ★ 第 18B 步：块层与可写 OBFS */
#include "block/block.h"
#include "block/virtio_blk.h"
#include "block/page_cache.h"
#include "block/journal.h"
#include "block/fsck.h"
#include "block/quota.h"
#include "block/xattr.h"
#include "block/symlink.h"
#include "block/crypto.h"
#include "block/obfs_rw.h"
#include "block/mkfs.h"
#include "block/block_selftest.h"

/* ★ 第 18C 步：用户态运行时与多用户基础 */
#include "user/user.h"
#include "user/signal.h"
#include "user/tty.h"
#include "user/account.h"
#include "user/elf_loader.h"
#include "user_test.h"

#include "futex.h"
#include "pipefs.h"
#include "select.h"
#include "seccomp.h"
#include "oom.h"
#include "ptrace.h"
#include "namespace.h"

extern void user_launcher_entry(void *arg);

extern const uint8_t  g_embedded_syscall_test[];
extern const uint64_t g_embedded_syscall_test_len;
extern const uint8_t  g_embedded_hello[];
extern const uint64_t g_embedded_hello_len;
extern const uint8_t  g_embedded_fs_test[];
extern const uint64_t g_embedded_fs_test_len;
extern const uint8_t  g_embedded_exception_test[];
extern const uint64_t g_embedded_exception_test_len;
/* ★ 第 18C 步新增内嵌程序 */
extern const uint8_t  g_embedded_signal_test[];
extern const uint64_t g_embedded_signal_test_len;
extern const uint8_t  g_embedded_pthread_test[];
extern const uint64_t g_embedded_pthread_test_len;
extern const uint8_t  g_embedded_dyn_test[];
extern const uint64_t g_embedded_dyn_test_len;
extern const uint8_t  g_embedded_libfoo[];
extern const uint64_t g_embedded_libfoo_len;

extern const uint8_t  g_embedded_pipe_test[];
extern const uint64_t g_embedded_pipe_test_len;

extern void user_test_driver_entry(void *arg);

/* ★ 第 18D 步 */
void test_18d(void);

#ifdef OB_QEMU_EXIT
extern void qemu_exit(uint32_t code);
#endif

extern uint8_t kernel_stack_top[];

static void banner(void)
{
    printk("Hello Kernel\n");
}

static volatile uint64_t g_a_iters = 0;
static volatile uint64_t g_b_iters = 0;

static void busy_wait(volatile int n)
{
    while (n-- > 0) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

static void thread_a(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; ++i) {
        g_a_iters++;
        serial_printf("[A] iter=%d (tid current=%llu)\n",
                      i,
                      (unsigned long long)sched_current()->tid);
        busy_wait(2000000);
    }
    serial_printf("[A] done\n");
    thread_exit();
}

static void thread_b(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; ++i) {
        g_b_iters++;
        serial_printf("[B] iter=%d\n", i);
        busy_wait(500000);
        thread_yield();
    }
    serial_printf("[B] done\n");
    thread_exit();
}

extern void net_tick(void);

static void thread_idle(void *arg)
{
    (void)arg;
    for (;;) {
        net_tick();

        /* ★★★ 修复 7：回收 orphan zombie ★★★
         *
         * 人工必须审查：
         *   - init/secmgr/servicehost/auditd/user-driver 等 parent_pid==0
         *     的 task 退出后无人 join，必须由 idle 线程回收。
         *   - task_reap_orphans 内部每次最多处理 8 个，避免长时占用。
         *   - idle 线程是唯一持有"回收 orphan"职责的线程。 */
        extern void task_reap_orphans(void);
        task_reap_orphans();

        __asm__ __volatile__("hlt");
    }
}

/* ============================================================
 * 把内嵌的用户程序写入根文件系统。
 * ============================================================ */
static int embed_install_one(const char *path,
                             const uint8_t *data, uint64_t len)
{
    (void)vfs_unlink(path);

    struct vfs_file *f = 0;
    int rc = vfs_open(path, VFS_O_CREAT | VFS_O_WRONLY, &f);
    if (rc != 0 || !f) {
        serial_printf("[EMBED] open '%s' failed rc=%d\n", path, rc);
        return rc ? rc : OB_EIO;
    }

    int64_t nw = vfs_write(f, data, len);
    vfs_close(f);

    if (nw != (int64_t)len) {
        serial_printf("[EMBED] write '%s' failed nw=%lld len=%llu\n",
                      path, (long long)nw, (unsigned long long)len);
        return OB_EIO;
    }
    serial_printf("[EMBED] installed '%s' (%llu bytes)\n",
                  path, (unsigned long long)len);
    return 0;
}

static void embed_install_programs(void)
{
    int rc = vfs_mkdir("/bin", 0755);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[EMBED] mkdir /bin rc=%d (continuing)\n", rc);
    }

    rc = vfs_mkdir("/tmp", 0777);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[EMBED] mkdir /tmp rc=%d (continuing)\n", rc);
    }

    rc = vfs_mkdir("/home", 0755);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[EMBED] mkdir /home rc=%d (continuing)\n", rc);
    }

    (void)embed_install_one("/bin/syscall_test.obr",
                            g_embedded_syscall_test,
                            g_embedded_syscall_test_len);
    (void)embed_install_one("/bin/hello.obr",
                            g_embedded_hello,
                            g_embedded_hello_len);
    (void)embed_install_one("/bin/fs_test.obr",
                            g_embedded_fs_test,
                            g_embedded_fs_test_len);
    (void)embed_install_one("/bin/exception_test.obr",
                            g_embedded_exception_test,
                            g_embedded_exception_test_len);
    /* ★ 第 18C 步新增 */
    (void)embed_install_one("/bin/signal_test.obr",
                            g_embedded_signal_test,
                            g_embedded_signal_test_len);
    (void)embed_install_one("/bin/pthread_test.obr",
                            g_embedded_pthread_test,
                            g_embedded_pthread_test_len);
    (void)embed_install_one("/bin/dyn_test.obr",
                            g_embedded_dyn_test,
                            g_embedded_dyn_test_len);
    (void)embed_install_one("/bin/pipe_test.obr",
                            g_embedded_pipe_test,
                            g_embedded_pipe_test_len);

    /* ★ 第 18C 步：为动态链接测试准备 /system/lib/ 目录 */
    (void)vfs_mkdir("/system", 0755);
    (void)vfs_mkdir("/system/lib", 0755);
    (void)embed_install_one("/system/lib/libfoo.obr",
                            g_embedded_libfoo,
                            g_embedded_libfoo_len);
}

void _kstart_c(struct ob_boot_info *bi)
{
    static struct ob_boot_info local_bi;
    if (bi) {
        uint8_t *src = (uint8_t *)bi;
        uint8_t *dst = (uint8_t *)&local_bi;
        for (size_t i = 0; i < sizeof(local_bi); ++i) dst[i] = src[i];
        bi = &local_bi;
    }

    if (!bi) {
        serial_printf("[KRN] no boot_info, halted\n");
        goto halt;
    }

    gdt_init();
    tss_init((uint64_t)(uintptr_t)kernel_stack_top);
    idt_init();
    pic_init();
    pit_init(100);

    lapic_init();
    ioapic_init();
    ipi_init();
    lapic_timer_init(100);

    percpu_init();
    pmm_init(bi);

    vmm_init();
    vmm_enable_smep_smap();
    serial_printf("[KRN] VMM stage done (CR3 + SMEP/SMAP)\n");

    slab_init();
    kmalloc_init();

    syscall_init();

    audit_init();
    see_init();
    ita_init();

    rng_init();
    {
        int ita2_rc = ita_sign_init();
        if (ita2_rc != 0) {
            serial_printf("[ITA2] WARN: sign subsystem not ready "
                          "(rc=%d), official .obr will be rejected\n",
                          ita2_rc);
        }
    }

    banner();

    void *p = kmalloc(64);
    if (!p) {
        printk(KERN_ERR "kmalloc(64) failed\n");
        goto fail;
    }
    for (int i = 0; i < 64; ++i) ((uint8_t *)p)[i] = (uint8_t)i;
    kfree(p);

    void *q = kzalloc(128);
    if (!q) {
        printk(KERN_ERR "kzalloc(128) failed\n");
        goto fail;
    }
    kfree(q);
    printk("kmalloc OK\n");

#if OB_STRESS_LEVEL != OB_STRESS_NONE
    {
        int rc = stress_run(OB_STRESS_LEVEL);
        if (rc != 0) {
            printk(KERN_ERR "[KRN] stress test (level %d) failed rc=%d\n",
                   OB_STRESS_LEVEL, rc);
            goto fail;
        }
    }
#endif

    #if OB_SELFTEST_STEP != OB_SELFTEST_NONE
        serial_printf("[KRN] running selftest step %d\n", OB_SELFTEST_STEP);
        selftest_run(OB_SELFTEST_STEP);
        serial_printf("[KRN] selftest step %d returned to _kstart_c\n",
                    OB_SELFTEST_STEP);
    #endif

    pid_init();
    task_init();
    permission_selftest();

    sha384_test();
    audit_test();
    see_test();
    see_policy_test();
    sandbox_test();
    obr_test();
    ita_test();

    sha512_test();
    ed25519_test();
    ita_sign_test();

    /* ★ 第 17 步：ITA 完整与 ART 预加载 */
    serial_printf("[KRN] step 17: ITA complete and ART preload\n");
    ita_manual_test();
    art_test();
    compat_preload_test();
    serial_printf("[KRN] WARN: step 17 shared memory is kernel-mode "
                  "approximation (no CPL=3 yet)\n");
    serial_printf("[KRN] WARN: step 17 ART uses SHA-384 truncated to 64-bit, "
                  "TODO replace with SHA-256 in step 18+\n");

    /* ★ 第 18 步：兼容层基础与异常转换 */
    serial_printf("[KRN] step 18: compat layer base and exception translation\n");
    sha256_test();
    compat_env_test();
    compat_path_test();
    compat_handle_test();
    compat_object_test();
    compat_exception_test();
    serial_printf("[KRN] WARN: step 18 does not introduce CPL=3 user mode; "
                  "compat layer is kernel-mode simulation\n");

    /* ★ 第 18A 步：网络协议栈基础 */
    serial_printf("[KRN] step 18A: network stack base\n");
    net_init();
    net_selftest();
    serial_printf("[KRN] WARN: step 18A uses kernel-mode sockets; "
                  "no CPL=3 user networking yet\n");

    /* ============================================================
     * ★ 第 18B 步：块层与可写持久文件系统
     * ============================================================ */
    vfs_init();

    block_init();
    pcache_init();
    journal_init();
    quota_init();
    xattr_init();
    symlink_init();
    crypto_init();

    if (virtio_blk_init() == 0) {
        struct block_device *bdev = virtio_blk_device();
        serial_printf("[BLOCK] VirtIO block device ready: %s "
                      "sectors=%llu\n",
                      bdev ? bdev->name : "(null)",
                      (unsigned long long)(bdev ? bdev->total_sectors : 0));

        if (bdev && !obfs_is_formatted(bdev, 0)) {
            uint64_t tb = (bdev->total_sectors * 512) / PAGE_SIZE;
            serial_printf("[BLOCK] image not formatted, running mkfs\n");
            int frc = obfs_format(bdev, 0, tb, 256);
            if (frc == 0) {
                serial_printf("[BLOCK] mkfs.obfs complete\n");
            } else {
                serial_printf("[BLOCK] mkfs.obfs failed rc=%d\n", frc);
            }
        }

        if (bdev) {
            uint64_t tb = (bdev->total_sectors * 512) / PAGE_SIZE;
            fsck_init();
            int frc = fsck_run(bdev, 0, tb, 1);
            if (frc == 0) {
                serial_printf("[FSCK] clean\n");
            } else if (frc > 0) {
                serial_printf("[FSCK] WARN: inconsistencies detected\n");
            }
        }
    } else {
        serial_printf("[BLOCK] no VirtIO block device found\n");
    }

    vfs_test();

    block_selftest();

    obfs_test();

    /* ============================================================
     * 根文件系统挂载：优先 OBFS-RW；回退 tmpfs
     * ============================================================ */
    {
        int root_mounted = 0;

        if (virtio_blk_ready()) {
            struct block_device *bdev = virtio_blk_device();
            if (bdev) {
                uint64_t tb = (bdev->total_sectors * 512) / PAGE_SIZE;
                struct vfs_superblock *obfs_sb =
                    obfs_rw_mount(bdev, 0, tb);
                if (obfs_sb) {
                    int rc = vfs_mount("/", obfs_sb);
                    if (rc == 0) {
                        serial_printf("[OBFS-RW] mounted root\n");
                        root_mounted = 1;
                    } else {
                        serial_printf("[OBFS-RW] root mount failed rc=%d, "
                                      "fallback to tmpfs\n", rc);
                        obfs_rw_umount(obfs_sb);
                    }
                } else {
                    serial_printf("[OBFS-RW] mount failed, "
                                  "fallback to tmpfs\n");
                }
            }
        }

        if (!root_mounted) {
            struct vfs_superblock *rootfs =
                tmpfs_mount("rootfs", 64ULL * 1024 * 1024);
            if (rootfs) {
                int rc = vfs_mount("/", rootfs);
                if (rc != 0) {
                    serial_printf("[KRN] root tmpfs mount failed rc=%d\n", rc);
                    tmpfs_umount(rootfs);
                } else {
                    serial_printf("[KRN] root tmpfs mounted, mounts=%u\n",
                                  (unsigned)vfs_mount_count());
                }
            }
        }
    }

    embed_install_programs();
    account_init();
    tty_init();
    critical_init();
    critical_test();
    oshell_run();

    uel_test();
    oshell_init();
    oshell_test();

    serial_printf("[KRN] step 15: privilege 0/1 isolation "
                  "(CPL=0 approximation)\n");
    mem_domain_test();
    priv_iso_test();

    serial_printf("[KRN] step 16: SEE complete and U-Sandbox\n");
    serial_printf("[KRN] WARN: step 16 does not introduce CPL=3 user mode; "
                  "sandbox isolation is approximate\n");

    /* ============================================================
     * ★ 第 18C 步：用户态运行时与多用户基础
     * ============================================================ */
    serial_printf("[KRN] step 18C: user-mode runtime and multiuser base\n");
    signal_init();
    user_init();
    elf_loader_init();
    user_test();
    serial_printf("[KRN] WARN: step 18C user-mode runtime is framework-only; "
                  "CPL=3 iretq path is implemented but not yet auto-triggered\n");

    sched_init();

    if (boot_services_default() != 0) {
        printk(KERN_ERR "[KRN] boot services failed\n");
        goto fail;
    }

    obinit_test();

    if (!thread_create("idle", thread_idle, 0, 0)) {
        printk(KERN_ERR "[KRN] cannot create idle thread\n");
        goto fail;
    }
    if (!thread_create("B", thread_b, 0, 3)) {
        printk(KERN_ERR "[KRN] cannot create thread B\n");
        goto fail;
    }
    if (!thread_create("A", thread_a, 0, 5)) {
        printk(KERN_ERR "[KRN] cannot create thread A\n");
        goto fail;
    }

    /* ============================================================
     * ★ 第 18C 步：启动用户态测试驱动内核线程。
     * ============================================================ */
    {
        static const char driver_name[] = "user-driver";
        struct task_t *dt = task_create(driver_name,
                                        user_test_driver_entry,
                                        0,
                                        0,
                                        0,
                                        5,
                                        0,
                                        0);
        if (!dt) {
            printk(KERN_WARN "[KRN] cannot create user-driver task; "
                             "user-mode verification skipped\n");
        } else {
            serial_printf("[KRN] user-test driver created: pid=%llu\n",
                          (unsigned long long)dt->pid);
        }
    }

    /* ============================================================
     * ★ 第 18D 步：IPC、资源控制与命名空间
     * ============================================================ */
    serial_printf("[KRN] step 18D: IPC, resource control and namespaces\n");
    futex_init();
    pipefs_init();
    select_init();
    seccomp_init();
    oom_init();
    ptrace_init();
    namespace_init();
    test_18d();   /* ★ 新增：执行 18D 自检 */
    serial_printf("[KRN] WARN: step 18D cross-process shm is "
                  "single-process approximation\n");

    printk("[KRN] starting scheduler...\n");
    sched_start();

fail:
#ifdef OB_QEMU_EXIT
    qemu_exit(1);
#endif
halt:
    for (;;) { __asm__ __volatile__("hlt"); }
}
/*===OmniBridgeOs/kernel/arch/x64/main.c 结束===*/