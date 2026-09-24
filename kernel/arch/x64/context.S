/*
 * 上下文切换汇编。
 * 编译目标：x86_64-pc-win32-coff（COFF / MS ABI）
 *
 * void context_switch(struct thread *prev, struct thread *next);
 *   RCX = prev（可为 NULL；本步调用者保证非 NULL）
 *   RDX = next
 *
 * 约定：
 *   - struct thread 的 rsp 字段位于偏移 0。
 *   - context_switch 只保存 callee-saved（rbx, rbp, r12..r15），
 *     调用者保存的寄存器由编译器自行负责。
 *   - 不切换 CR3：本步所有内核线程共享同一地址空间。后续若引入
 *     每个线程独立 CR3，可在 mov %rsp 之前/之后加 CR3 切换。
 *   - 不切换 FPU：当前内核以 -mno-sse -mno-mmx 编译，不使用 FPU。
 *     后续若启用浮点，应在保存 GP 寄存器后调用 fxsave prev->fpu_state，
 *     并在恢复前调用 fxrstor next->fpu_state。
 */

.text

.globl context_switch
context_switch:
    /* --- 保存 prev 的 callee-saved 寄存器 --- */
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15

    /* prev->rsp = rsp （偏移 0） */
    mov  %rsp, 0(%rcx)

    /* rsp = next->rsp */
    mov  0(%rdx), %rsp

    /* --- 恢复 next 的 callee-saved 寄存器 --- */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx

    ret

/*
 * 新线程的启动跳板。
 *
 * 首次进入时约定：
 *   r15 = 线程入口函数地址
 *   r14 = 入口参数
 *   rsp  = 由 thread_create 构造的“假返回现场”经过 context_switch
 *          的 ret 之后指向 thread_start_trampoline。
 *
 * 目的：
 *   1) 保证 MS ABI 要求的栈 16 字节对齐（call 前 RSP % 16 == 0）。
 *   2) 为入口函数预留 32 字节 shadow space。
 *   3) 主动开启中断：新线程不经过 iretq，必须自己 sti。
 *   4) 入口函数返回后进入死循环。
 */
.globl thread_start_trampoline
thread_start_trampoline:
    and  $-16, %rsp              /* 强制 16 字节对齐 */
    sub  $32, %rsp               /* MS ABI shadow space */

    /* 先开中断，再调用入口函数；入口函数即使执行时间较长也不会
     * 让系统失去定时器节拍。 */
    sti

    mov  %r14, %rcx              /* arg 作为 MS ABI 第一个参数 */
    call *%r15                   /* 调用线程入口 */

    /* 入口函数不应返回；若返回则挂起该线程的 CPU */
1:  hlt
    jmp  1b