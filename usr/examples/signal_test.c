/*===OmniBridgeOs/usr/examples/signal_test.c===*/
/*
 * signal_test.c —— 信号框架验收测试。
 *
 * 验收对应（第 18C 步）：
 *   ✓ 用户态除零触发 SIGFPE，注册的处理函数被调用。
 *   ✓ 用户态异常不导致内核 panic，而是转换为信号。
 *   ✓ 信号 handler 返回后，内核跳过触发故障的指令，
 *     程序从下一条指令继续执行。
 *
 * 预期输出（串口）：
 *   [signal_test] registering SIGFPE handler
 *   [signal_test] triggering divide-by-zero
 *   [EXC] user-mode vec=0 -> signal pid=NNNN
 *   [SIGNAL] exception vec=0 -> sig=8 pid=NNNN rip=0xNNNN
 *   [SIGNAL] fault_rip=0xNNNN insn_len=3 (sigreturn will skip)
 *   [SIGNAL] deliver sig=8 to pid=NNNN handler=0xNNNN rip=0xNNNN rsp=0xNNNN
 *   [signal_test] handler: SIGFPE caught
 *   [SIGNAL] fixup: advance rip 0xNNNN -> 0xNNNN (skip fault insn, len=3)
 *   [signal_test] returned from handler safely
 *   [signal_test] OK
 *   [TASK] exit pid=NNNN code=0
 *
 * 实现要点（人工必须审查）：
 *   - handler 只记录 g_caught = signum，不修改任何 ucontext；
 *   - handler 返回时通过 sigreturn trampoline 触发 SYS_OB_SigReturn；
 *   - 内核 signal_fixup_resume 检测到 handler 未修改 resume.rip，
 *     把 RIP 推进到触发 #DE 的 `div` 指令之后（fault_insn_len=3），
 *     程序从 y 赋值的下一条指令继续；
 *   - main 检测 g_caught == SIGFPE 后继续执行并返回 0。
 */
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/unistd.h"
#include "../include/ob/signal.h"

static volatile int g_caught = 0;

static void fpe_handler(int signum)
{
    g_caught = signum;
    printf("[signal_test] handler: SIGFPE caught\n");
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[signal_test] registering SIGFPE handler\n");
    if (signal(SIGFPE, fpe_handler) == SIG_ERR) {
        printf("[signal_test] FAIL: signal() registration\n");
        return 1;
    }

    printf("[signal_test] triggering divide-by-zero\n");

    volatile int zero = 0;
    volatile int x = 1;

    /* 这一行触发 #DE → SIGFPE → fpe_handler；
     * 内核在 sigreturn 时把 RIP 推进到下一条指令，
     * 因此函数从这里继续执行。 */
    volatile int y = x / zero;
    (void)y;

    if (g_caught != SIGFPE) {
        printf("[signal_test] FAIL: handler not invoked (g_caught=%d)\n",
               g_caught);
        return 1;
    }

    printf("[signal_test] returned from handler safely\n");
    printf("[signal_test] OK\n");
    return 0;
}
/*===OmniBridgeOs/usr/examples/signal_test.c 结束===*/