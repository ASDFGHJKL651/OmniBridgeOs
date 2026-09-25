/*===OmniBridgeOs/usr/examples/libfoo.c===*/
/*
 * libfoo.c —— 共享库示例（供 dyn_test 依赖）。
 *
 * 说明（人工必须审查）：
 *   - libfoo.obr 不通过 crt0 启动，仅由 elf_load_deps 在 dyn_test
 *     加载阶段映射到进程地址空间。
 *   - obcc.sh 仍会用 lld-link 生成 PE32+，需要满足入口符号约束。
 *     因为 Makefile 对该库传 --no-libc（不链接 crt0/oblibc），
 *     故本文件须自行提供 _start 占位；它永远不会被执行。
 *   - __attribute__((used)) 防止 -O2 把空函数优化掉。
 */
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"

__attribute__((used))
void _start(void)
{
    /* 占位：libfoo.obr 作为共享库加载，入口不会被调用 */
    for (;;) { }
}

int foo_add(int a, int b) { return a + b; }
int foo_mul(int a, int b) { return a * b; }
/*===OmniBridgeOs/usr/examples/libfoo.c 结束===*/