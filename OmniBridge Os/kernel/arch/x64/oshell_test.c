/*===OmniBridgeOs/kernel/arch/x64/oshell_test.c===*/
#include "oshell_test.h"
#include "oshell.h"
#include "vfs.h"
#include "tmpfs.h"
#include "task.h"
#include "serial.h"
#include "kmalloc.h"
#include "account.h"
#include "tty.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[OSHELL-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[OSHELL-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static void write_minimal_obr(const char *path)
{
    uint8_t img[256];
    for (unsigned i = 0; i < sizeof(img); ++i) img[i] = 0;

    uint32_t magic = 0x4F425220u;
    for (int i = 0; i < 4; ++i)
        img[i] = (uint8_t)((magic >> (8 * i)) & 0xFF);
    img[4] = 0x00; img[5] = 0x01;
    img[6] = 0x01;
    img[7] = 0x00;
    uint64_t entry = 80 + 56;
    for (int i = 0; i < 8; ++i)
        img[8 + i] = (uint8_t)((entry >> (8 * i)) & 0xFF);
    uint64_t ph_off = 80;
    for (int i = 0; i < 8; ++i)
        img[16 + i] = (uint8_t)((ph_off >> (8 * i)) & 0xFF);
    img[24] = 0x01; img[25] = 0x00;

    uint8_t *ph = img + 80;
    ph[0] = 0x01; ph[1] = 0; ph[2] = 0; ph[3] = 0;
    ph[4] = 0x05; ph[5] = 0; ph[6] = 0; ph[7] = 0;
    uint64_t off = 136;
    for (int i = 0; i < 8; ++i)
        ph[8 + i] = (uint8_t)((off >> (8 * i)) & 0xFF);
    ph[32] = 0x08;
    ph[40] = 0x08;

    uint64_t payload = 0xDEADBEEFCAFEBABEULL;
    for (int i = 0; i < 8; ++i)
        img[136 + i] = (uint8_t)((payload >> (8 * i)) & 0xFF);

    struct vfs_file *f = 0;
    if (vfs_open(path, VFS_O_CREAT | VFS_O_RDWR, &f) != 0) return;
    vfs_write(f, img, sizeof(img));
    vfs_close(f);
}

void oshell_test(void)
{
    failures = 0;
    serial_printf("[OSHELL-TEST] === begin ===\n");

    /* ---------- 1) 分词 ---------- */
    {
        char buf[256];
        char *argv[16];
        int argc;

        argc = oshell_tokenize("ls /system", buf, sizeof(buf),
                               argv, 16);
        check("tokenize simple",
              argc == 2 &&
              argv[0][0] == 'l' && argv[0][1] == 's' && argv[0][2] == '\0' &&
              argv[1][0] == '/');

        argc = oshell_tokenize("  echo a b  ", buf, sizeof(buf),
                               argv, 16);
        check("tokenize trim",
              argc == 3 &&
              argv[0][0] == 'e' && argv[1][0] == 'a' && argv[2][0] == 'b');

        argc = oshell_tokenize("   ", buf, sizeof(buf), argv, 16);
        check("empty line", argc == 0);
    }

    /* ---------- 2) 命令注册 ---------- */
    {
        static const char *required[] = {
            "ls", "cd", "pwd", "cat", "echo", "rm", "mkdir", "rmdir",
            "cp", "mv", "chmod", "chown", "ps", "kill", "perm",
            "audit", "meminfo", "drivers", "help", "exit",
            "sandbox", "obctl",
            /* ★ 第 18C 步新增 */
            "jobs", "fg", "bg",
            "login", "su", "passwd", "whoami", "id",
        };
        for (unsigned k = 0; k < sizeof(required) / sizeof(required[0]); ++k) {
            int found = 0;
            for (uint32_t i = 0; i < g_oshell_cmd_count; ++i) {
                const char *a = g_oshell_cmds[i].name;
                const char *b = required[k];
                int eq = 1;
                while (*a && *b) { if (*a != *b) { eq = 0; break; } ++a; ++b; }
                if (eq && *a == '\0' && *b == '\0') { found = 1; break; }
            }
            char desc[64];
            const char *pfx = "command '";
            int n = 0;
            while (pfx[n]) { desc[n] = pfx[n]; ++n; }
            const char *r = required[k];
            while (*r && n < 60) desc[n++] = *r++;
            const char *sfx = "' registered";
            while (*sfx && n < 62) desc[n++] = *sfx++;
            desc[n] = '\0';
            check(desc, found);
        }
    }

    /* ---------- 3) 挂载独立 tmpfs ---------- */
    struct vfs_superblock *sb = tmpfs_mount("oshtest", 4 * 1024 * 1024);
    check("tmpfs_mount", sb != 0);
    if (!sb) goto done;

    int rc = vfs_mount("/oshtest", sb);
    check("vfs_mount /oshtest", rc == 0);

    /* ---------- 4) 端到端脚本 ---------- */
    oshell_init();
    struct oshell_ctx *ctx = oshell_get_ctx();
    ctx->task = task_find_by_pid(PID_IDLE);

    rc = oshell_exec_line(ctx, "pwd");
    check("pwd ok", rc == 0);
    check("cwd == '/'",
          ctx->cwd[0] == '/' && ctx->cwd[1] == '\0');

    rc = oshell_exec_line(ctx, "mkdir /oshtest/d1");
    check("mkdir /oshtest/d1", rc == 0);

    struct vfs_inode *d1 = 0;
    rc = vfs_lookup("/oshtest/d1", &d1);
    check("lookup /oshtest/d1", rc == 0 && d1 && VFS_S_ISDIR(d1->mode));

    rc = oshell_exec_line(ctx, "cd /oshtest/d1");
    check("cd /oshtest/d1", rc == 0);
    check("cwd == /oshtest/d1",
          ctx->cwd[0] == '/' && ctx->cwd[1] == 'o');

    rc = oshell_exec_line(ctx, "echo hello world");
    check("echo hello world", rc == 0);

    rc = oshell_exec_line(ctx, "cd ..");
    check("cd ..", rc == 0);
    check("cwd == /oshtest",
          ctx->cwd[0] == '/' && ctx->cwd[1] == 'o' &&
          ctx->cwd[7] == 't' && ctx->cwd[8] == '\0');

    rc = oshell_exec_line(ctx, "echo");
    check("echo (no args)", rc == 0);

    /* ---------- 4b) cp / mv / rm / rmdir ---------- */
    {
        struct vfs_file *wf = 0;
        rc = vfs_open("/oshtest/d1/f1", VFS_O_CREAT | VFS_O_RDWR, &wf);
        check("create /oshtest/d1/f1", rc == 0 && wf != 0);
        if (rc == 0 && wf) {
            const char *msg = "hello cp";
            int64_t nw = vfs_write(wf, msg, 8);
            check("write /oshtest/d1/f1", nw == 8);
            vfs_close(wf);
        }

        rc = oshell_exec_line(ctx, "cp /oshtest/d1/f1 /oshtest/d1/f2");
        check("cp /oshtest/d1/f1 -> /oshtest/d1/f2", rc == 0);

        struct vfs_inode *f2_ino = 0;
        rc = vfs_lookup("/oshtest/d1/f2", &f2_ino);
        check("lookup /oshtest/d1/f2", rc == 0 && f2_ino != 0);
        if (rc == 0 && f2_ino) {
            check("f2 size == 8", f2_ino->size == 8);
        }

        rc = oshell_exec_line(ctx, "mv /oshtest/d1/f2 /oshtest/d1/f3");
        check("mv /oshtest/d1/f2 -> /oshtest/d1/f3", rc == 0);

        struct vfs_inode *tmp = 0;
        rc = vfs_lookup("/oshtest/d1/f2", &tmp);
        check("f2 gone after mv", rc != 0);
        tmp = 0;
        rc = vfs_lookup("/oshtest/d1/f3", &tmp);
        check("f3 exists after mv", rc == 0 && tmp != 0);

        rc = oshell_exec_line(ctx, "rm /oshtest/d1/f3");
        check("rm /oshtest/d1/f3", rc == 0);
        tmp = 0;
        rc = vfs_lookup("/oshtest/d1/f3", &tmp);
        check("f3 gone after rm", rc != 0);

        rc = oshell_exec_line(ctx, "rmdir /oshtest/d1");
        check("rmdir non-empty fails", rc != 0);

        rc = oshell_exec_line(ctx, "rm /oshtest/d1/f1");
        check("rm /oshtest/d1/f1", rc == 0);

        rc = oshell_exec_line(ctx, "rmdir /oshtest/d1");
        check("rmdir /oshtest/d1 after clear", rc == 0);

        struct vfs_inode *d1_after = 0;
        rc = vfs_lookup("/oshtest/d1", &d1_after);
        check("d1 gone after rmdir", rc != 0);
    }

    /* ---------- 5) 权限链路 ---------- */
    {
        struct task_t fake;
        uint8_t *p = (uint8_t *)&fake;
        for (unsigned i = 0; i < sizeof(fake); ++i) p[i] = 0;
        fake.pid = 2000;
        fake.privilege_level = 5;
        fake.security_token.level = 5;

        struct oshell_ctx saved = *ctx;
        ctx->task = &fake;

        rc = oshell_exec_line(ctx, "kill 0");
        check("kill pid 0 denied by check_pid_access", rc == OB_EPERM);

        ctx->task = saved.task;
    }

    /* ---------- 6) obrun 演示 ---------- */
    {
        rc = oshell_exec_line(ctx, "mkdir /oshtest/bin");
        check("mkdir /oshtest/bin", rc == 0);

        write_minimal_obr("/oshtest/bin/hello.obr");

        struct vfs_inode *hf = 0;
        rc = vfs_lookup("/oshtest/bin/hello.obr", &hf);
        check("hello.obr exists", rc == 0 && hf != 0);

        rc = oshell_exec_line(ctx, "obrun /oshtest/bin/hello.obr");
        check("obrun returns 0", rc == 0);
    }

    /* ---------- 7) obctl 命令存在性测试 ---------- */
    {
        struct task_t fake;
        uint8_t *p = (uint8_t *)&fake;
        for (unsigned i = 0; i < sizeof(fake); ++i) p[i] = 0;
        fake.pid = 5000;
        fake.privilege_level = 8;
        fake.security_token.level = 8;

        struct oshell_ctx saved = *ctx;
        ctx->task = &fake;

        rc = oshell_exec_line(ctx, "obctl trust add /x");
        check("obctl trust add missing hash -> -EINVAL", rc == OB_EINVAL);

        rc = oshell_exec_line(ctx,
            "obctl trust add /x --hash "
            "000000000000000000000000000000000000000000000000"
            "000000000000000000000000000000000000000000000000");
        check("obctl trust add priv8 -> -EPERM", rc == OB_EPERM);

        rc = oshell_exec_line(ctx, "obctl trust list");
        check("obctl trust list ok", rc == 0);

        ctx->task = saved.task;
    }

    /* ---------- 7b) ★ 第 18C 步：多用户命令 ---------- */
    {
        rc = oshell_exec_line(ctx, "login root root");
        check("login root", rc == 0);

        rc = oshell_exec_line(ctx, "whoami");
        check("whoami", rc == 0);

        rc = oshell_exec_line(ctx, "id");
        check("id", rc == 0);

        rc = oshell_exec_line(ctx, "login root wrongpass");
        check("login wrong password rejected", rc != 0);

        rc = oshell_exec_line(ctx, "passwd user newpass");
        check("passwd user", rc == 0);

        rc = oshell_exec_line(ctx, "login user newpass");
        check("login user with new password", rc == 0);
    }

    /* ---------- 7c) ★ 第 18C 步：作业控制命令存在性 ---------- */
    {
        rc = oshell_exec_line(ctx, "jobs");
        check("jobs command exists", rc == 0);

        rc = oshell_exec_line(ctx, "fg");
        check("fg without arg -> -EINVAL", rc == OB_EINVAL);

        rc = oshell_exec_line(ctx, "bg");
        check("bg without arg -> -EINVAL", rc == OB_EINVAL);
    }

    /* ---------- 8) umount ---------- */
    rc = vfs_umount("/oshtest");
    check("vfs_umount /oshtest", rc == 0);

done:
    if (failures == 0) {
        serial_printf("[OSHELL-TEST] selftest OK\n");
    } else {
        serial_printf("[OSHELL-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[OSHELL-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/oshell_test.c 结束===*/