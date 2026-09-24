/*===OmniBridgeOs/usr/lib/oblibc/crt0.S===*/
/*
 * 用户态启动代码（crt0）+ 信号返回 trampoline。
 *
 * 入口约定（由内核 user_enter() 建立）：
 *   用户栈：
 *       [rsp +  0]  argv (char **)
 *       [rsp +  8]  argc (int)
 *
 * %fs 已由内核指向 TLS 页，且内核在进入用户态之前已把 tls_base
 * 值预填到 TLS 页的前 8 字节 —— 因此 %fs:0 已经是正确的
 * tls_block* 自身（pthread.c 的 tls_self() 读取 %fs:0）。
 *
 * 编译目标：x86_64-pc-win32-coff（MS ABI）。
 */
.text
.globl _start
_start:
    /* 读取 argc/argv */
    movq  8(%rsp), %r12          /* argc */
    movq  0(%rsp), %r13          /* argv */

    /* ★ 第 18C 步：TLS 自引用已由内核在 user_setup 阶段预填。
     * 这里只做防御性校验（%fs:0 应当非 0）。 */
    movq  %fs:0, %rax
    testq %rax, %rax
    jnz   .Ltls_ok
    /* 若为 0（不应发生），无法在用户态修复（无法读 MSR），继续执行 */
.Ltls_ok:

    /* 对齐栈到 16 字节，预留 MS ABI shadow space */
    andq  $-16, %rsp
    subq  $32, %rsp

    movq  %r12, %rcx             /* arg1: argc */
    movq  %r13, %rdx             /* arg2: argv */
    xorq  %r8,  %r8              /* arg3: envp = NULL */
    call  main

    /* main 返回值 -> exit code */
    movq  %rax, %rdi
    movq  $0x130, %rax           /* SYS_OB_UserExit */
    syscall

.Lhalt:
    hlt
    jmp   .Lhalt

/* ------------------------------------------------------------
 * ob_sigreturn_trampoline —— 用户态信号返回路径。
 *
 * 语义：
 *   当用户态信号 handler 执行 return 时，其返回地址即为此 trampoline。
 *   本 trampoline 通过 SYS_OB_SigReturn 让内核恢复信号前的上下文
 *   （内核会 iretq 回原 RIP/RSP）。
 * ------------------------------------------------------------ */
.globl ob_sigreturn_trampoline
ob_sigreturn_trampoline:
    movq  $0x133, %rax           /* SYS_OB_SigReturn */
    syscall
    /* 若 sigreturn 失败，则退出 */
    movq  $0x130, %rax           /* SYS_OB_UserExit */
    xorq  %rdi, %rdi
    syscall
.Lhalt2:
    hlt
    jmp   .Lhalt2
/*===OmniBridgeOs/usr/lib/oblibc/crt0.S 结束===*/