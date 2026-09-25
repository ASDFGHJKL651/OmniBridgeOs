/*===OmniBridgeOs/usr/examples/linux_hello.c===*/
/*
 * linux_hello.c —— 静态链接的 Linux ELF64 hello world。
 *
 * ★ 第 19 步（clang 直出方案）：本文件不依赖任何 libc。使用内联汇编
 *   直接发出 Linux x86_64 syscall（write / exit），因此可以用
 *   clang --target=x86_64-linux-gnu -nostdlib -static 一条命令直出
 *   ELF64，无需安装 musl-gcc 或 x86_64-linux-gnu-gcc。
 *
 * 编译命令（由 Makefile 自动执行；此处仅作文档）：
 *
 *   clang --target=x86_64-linux-gnu \
 *         -nostdlib -static -no-pie \
 *         -fno-pic -fno-stack-protector \
 *         -fuse-ld=lld \
 *         -o linux_hello.elf linux_hello.c
 *
 * 输出特征（人工必须审查）：
 *   - ELF64 ET_EXEC，e_machine = EM_X86_64 (62)；
 *   - 入口点符号 _start，通过 ld.lld 的默认链接脚本生成；
 *   - 无 PT_INTERP（-static 保证）；内核 ELF64 加载器可接受；
 *   - 无 PT_DYNAMIC（-nostdlib 保证）。
 *
 * 人工必须审查（内联汇编注意点）：
 *   - syscall 指令破坏 RCX 与 R11，clobber list 必须包含它们；
 *   - 参数寄存器顺序：RDI/RSI/RDX/R10/R8/R9，本文件仅用 3 个；
 *   - 返回值为负 errno 时本程序不做重试（教学示例）。
 */

typedef unsigned long  size_t_u;

/* Linux x86_64 syscall 号 */
#define SYS_write  1
#define SYS_exit   60

/* syscall(nr, a1) —— 单参数 */
static long lx_syscall1(long nr, long a1)
{
    long ret;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory");
    return ret;
}

/* syscall(nr, a1, a2, a3) —— 三参数 */
static long lx_syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

static size_t_u lx_strlen(const char *s)
{
    size_t_u n = 0;
    while (s[n]) ++n;
    return n;
}

/*
 * 入口点。ld.lld 默认把 _start 作为 ELF entry。
 *
 * 人工必须审查：
 *   - Linux x86_64 ABI 要求进入用户态时 RSP 指向 argc；
 *     但我们不使用 argc/argv，忽略即可。
 *   - exit() 不返回，for(;;) 只做防御。
 */
void _start(void)
{
    const char *msg = "Hello from Linux ELF!\n";
    lx_syscall3(SYS_write, 1 /* stdout */, (long)msg, (long)lx_strlen(msg));
    lx_syscall1(SYS_exit, 0);

    for (;;) { }
}

/* 显式占位符号，防止 -nostdlib 下某些工具链插入的默认桩 */
void _init(void)  { }
void _fini(void)  { }
/*===OmniBridgeOs/usr/examples/linux_hello.c 结束===*/