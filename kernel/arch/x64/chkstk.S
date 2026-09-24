/*===OmniBridgeOs/kernel/arch/x64/chkstk.S===*/
/*
 * Windows x64 ABI 栈探测桩 __chkstk。
 *
 * 背景（人工必须审查）：
 *   clang 在 x86_64-pc-win32-coff 目标下编译某个函数时，如果该函数
 *   在栈上分配超过 4KB 的局部存储（数组 / 大结构体等），会在函数
 *   序言中插入：
 *       mov  $N, %rax
 *       call __chkstk
 *       sub  %rax, %rsp
 *   其目的是逐页触碰即将使用的栈空间，让 OS 有机会按需扩展栈。
 *
 *   本内核没有 Windows guard page 机制，但必须提供 __chkstk 符号，
 *   否则 lld-link 会报告 "undefined symbol: __chkstk"。
 *   为了尽早发现栈溢出，本实现做真正意义上的逐页触碰：一旦触碰
 *   到未映射的地址，会立即触发 #PF，便于在 QEMU 中定位问题，而不是
 *   静默地破坏相邻内存。
 *
 * 约定（x64 Windows ABI / MS ABI）：
 *   输入：RAX = 需要探测的字节数
 *   输出：无（RAX 值不变）
 *   破坏：R10、R11（volatile 寄存器，调用方不依赖其值）
 *   保留：其它通用寄存器、RSP（本实现不修改 RSP）
 *
 * 算法：
 *   r11 = rsp - rax                    （目标最低地址）
 *   从 rsp - 4096 开始，每页触碰一次，直到 r10 < r11
 *   最后触碰 r11 所在页，保证最低地址也被覆盖
 *
 * 注意：
 *   - 使用 testq $0, (%r10) 而不是 movb $0, (%r10)：
 *     testq 是只读操作，不修改内存内容，仅触发内存访问；
 *     movb 会写入 0，可能破坏栈上其它数据。
 *   - 本实现不写任何内存，因此对栈上已存在的数据无副作用。
 *   - 如果 RAX == 0，r11 == rsp；循环直接跳到 label 2，触碰 rsp 处
 *     的内存（已映射，无害）。
 *
 * 目标：COFF / x86_64-pc-win32-coff / MS ABI
 */

.text
.globl __chkstk
__chkstk:
    mov  %rsp, %r11
    sub  %rax, %r11              /* r11 = rsp - rax = 目标最低地址 */

    mov  %rsp, %r10
1:  sub  $4096, %r10
    cmp  %r11, %r10
    jb   2f                      /* r10 < r11 → 已覆盖到最低地址，退出 */
    testq $0, (%r10)             /* 触碰该页 */
    jmp  1b

2:  testq $0, (%r11)             /* 触碰最低地址所在页 */
    ret
/*===OmniBridgeOs/kernel/arch/x64/chkstk.S 结束===*/