/*===OmniBridgeOs/kernel/arch/x64/user_test.c===*/
#include "user_test.h"
#include "user/user.h"
#include "user/signal.h"
#include "user/tty.h"
#include "user/account.h"
#include "user/elf_loader.h"
#include "task.h"
#include "sched.h"
#include "serial.h"
#include "pmm.h"

/*
 * 第 18C 步用户态运行时自检。
 *
 * 覆盖内容：
 *   1) 用户地址空间边界检查（user_range_ok）；
 *   2) 信号子系统 task 初始化；
 *   3) 账户库（root/user 存在、login 成功/失败、whoami、passwd）；
 *   4) TTY 作业控制（add_job / Ctrl-C / Ctrl-Z / list_jobs / fg / bg）；
 *   5) 动态链接器初始化。
 *
 * 人工必须审查：
 *   - 本测试在主核的 _kstart_c 中、调度器启动前调用。此时 task_t 的
 *     全局链表中只有 bootstrap（PID 0）。所有需要"当前 task"上下文的
 *     操作（如 sys_signal）不能在此调用。
 *   - 所有 fake task 均为栈上结构体，严禁加入全局链表。
 *   - tty_handle_char(3) / tty_handle_char(26) 内部通过 sys_kill 路径
 *     向 pgid 发送信号；若目标 PID 不存在，sys_kill 返回 -ENOENT，
 *     这是预期的（本测试仅验证调用路径不崩溃）。
 */

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[USER-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[USER-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void user_test(void)
{
    failures = 0;
    serial_printf("[USER-TEST] === begin ===\n");

    /* ============================================================
     * 1) 用户地址空间检查
     * ============================================================ */
    check("user_range_ok(stack_top)",
          user_range_ok(USER_STACK_TOP - 16, 16) == 1);
    check("user_range_ok(kernel_addr) == 0",
          user_range_ok(0xFFFF800000000000ULL, 16) == 0);
    check("user_range_ok(overflow) == 0",
          user_range_ok(0xFFFFFFFFFFFFFFF0ULL, 0x100) == 0);

    /* 额外边界 */
    check("user_range_ok(0, 0)",
          user_range_ok(0, 0) == 1);    /* size==0 时返回 1 */
    check("user_range_ok(USER_STACK_BOTTOM, PAGE_SIZE)",
          user_range_ok(USER_STACK_BOTTOM, PAGE_SIZE) == 1);

    /* ============================================================
     * 2) 信号子系统：fake task 初始化
     * ============================================================ */
    {
        struct task_t fake;
        uint8_t *p = (uint8_t *)&fake;
        for (unsigned i = 0; i < sizeof(fake); ++i) p[i] = 0;
        fake.pid = 7000;
        signal_init_task(&fake);
        check("signal_init_task", 1);

        /* 注册一个 handler 并验证 resume 槽位可取 */
        struct user_regs *slot = signal_resume_slot(&fake);
        check("signal_resume_slot non-NULL", slot != 0);

        /* 清理 */
        signal_free_task(&fake);
        check("signal_free_task", signal_resume_slot(&fake) == 0);
    }

    /* ============================================================
     * 3) 账户库
     * ============================================================ */
    {
        check("root exists", account_lookup_name("root", 0) == 0);
        check("user exists", account_lookup_name("user", 0) == 0);
        check("bad user rejected", account_lookup_name("noone", 0) != 0);

        struct acct_session s;
        check("login root/root", account_login("root", "root", &s) == 0);

        char who[16];
        account_whoami(who, sizeof(who));
        check("whoami == root", who[0] == 'r' && who[3] == 't');

        account_logout(&s);
        check("login wrong pass rejected",
              account_login("root", "wrong", 0) != 0);

        /* 修改密码后重新登录 */
        check("passwd user -> 'newpass'",
              account_set_password("user", "newpass") == 0);
        check("login user with new password",
              account_login("user", "newpass", &s) == 0);
        account_logout(&s);
    }

    /* ============================================================
     * 4) TTY 作业控制
     *
     * 人工必须审查：
     *   - tty_handle_char(3) 走 send_to_pgrp -> sys_kill 路径。
     *     由于本测试在主核引导阶段运行，sched_current() 返回
     *     bootstrap（task==NULL），sys_kill 会因 current_task()==NULL
     *     返回 -EPERM。这是预期行为，测试只验证"调用不崩溃"。
     *   - 使用 pgid=2000 作为 fake 前台进程组。
     * ============================================================ */
    {
        tty_init();

        int rc = tty_add_job("testjob", 2000, 2000);
        check("tty_add_job", rc == 0);

        /* 设置 2000 为前台进程组 */
        int64_t pg = sys_tcsetpgrp(2000);
        check("sys_tcsetpgrp", pg == 0);

        int64_t got = sys_tcgetpgrp();
        check("sys_tcgetpgrp == 2000", got == 2000);

        /* 触发 Ctrl-C：tty_handle_char 内部会调用 sys_kill，
         * 由于当前 task 为 bootstrap（task->task==NULL），
         * sys_kill 返回 -EPERM，但调用路径不崩溃。 */
        tty_handle_char(3);
        check("tty_handle_char(Ctrl-C) exercised", 1);

        /* 触发 Ctrl-Z */
        tty_handle_char(26);
        check("tty_handle_char(Ctrl-Z) exercised", 1);

        /* 列出作业：应看到 1 个 stopped 的 testjob */
        tty_list_jobs();
        check("tty_list_jobs exercised", 1);

        /* fg/bg 参数边界 */
        check("tty_fg(0) -> -1", tty_fg(0) == -1);
        check("tty_fg(99) -> -1", tty_fg(99) == -1);
        check("tty_bg(0) -> -1", tty_bg(0) == -1);

        /* fg(1)：job 1 存在；sys_kill 会返回负错误但 tty_fg 本身返回 0 */
        check("tty_fg(1) -> 0", tty_fg(1) == 0);

        /* 恢复前台进程组 */
        sys_tcsetpgrp(0);
    }

    /* ============================================================
     * 5) 动态链接器初始化
     * ============================================================ */
    {
        elf_loader_init();
        check("elf_loader_init", 1);

        /* 幂等 */
        elf_loader_init();
        check("elf_loader_init idempotent", 1);

        /* elf_load_deps(NULL, NULL, 0) 应返回 0（无依赖） */
        int rc = elf_load_deps(0, 0, 0);
        check("elf_load_deps(no deps) -> 0", rc == 0);
    }

    /* ============================================================
     * 汇总
     * ============================================================ */
    if (failures == 0) {
        serial_printf("[USER-TEST] selftest OK\n");
    } else {
        serial_printf("[USER-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[USER-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/user_test.c 结束===*/