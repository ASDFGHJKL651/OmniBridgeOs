/*===OmniBridgeOs/usr/examples/dyn_test.c===*/
/*
 * dyn_test.c —— 动态链接验收。
 *
 * 18C 阶段验收标准（降级）：
 *   - elf_load_deps 打开 /system/lib/libfoo.obr 并映射成功；
 *   - dyn_test 通过 --dep 声明对 libfoo.obr 的依赖；
 *   - foo_add 在同库内以静态方式可用（跨库符号解析留 18D）。
 *
 * 预期输出：
 *   [DYN] mapping library 'libfoo.obr' from /system/lib/...
 *   [DYN] mapped lib 'libfoo.obr' at 0x700000000000
 *   [DYN] loaded 1/1 dependencies
 *   [dyn_test] calling foo_add(2,3)
 *   [dyn_test] foo_add(2,3)=5
 *   [dyn_test] OK
 *   [TASK] exit pid=NNNN code=0
 */
#include "../include/ob/ob.h"
#include "../include/ob/stdio.h"
#include "../include/ob/stdlib.h"

/* 降级：同库内静态定义，仅用于验证 elf_load_deps 端到端调用路径。
 * 18D 会替换为 extern int foo_add(int, int); */
int foo_add(int a, int b)
{
    return a + b;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[dyn_test] calling foo_add(2,3)\n");
    int r = foo_add(2, 3);
    printf("[dyn_test] foo_add(2,3)=%d\n", r);

    if (r != 5) {
        printf("[dyn_test] FAIL: expected 5, got %d\n", r);
        return 1;
    }
    printf("[dyn_test] OK\n");
    return 0;
}
/*===OmniBridgeOs/usr/examples/dyn_test.c 结束===*/