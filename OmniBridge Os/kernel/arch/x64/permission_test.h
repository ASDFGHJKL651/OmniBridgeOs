#ifndef OMNIBRIDGE_PERMISSION_TEST_H
#define OMNIBRIDGE_PERMISSION_TEST_H

/* 第 9 步权限引擎自检。
 *
 * 设计要点（人工必须审查）：
 *   - 使用纯栈上 fake task，绝不调用 task_create。
 *     task_create 会污染 PID 位图、kmalloc 缓存、就绪队列，并改变
 *     PMM free_count，破坏启动日志的 free pages 判据。
 *   - 断言方式为"结果符号"比较：expected==0 要求 rc==0；
 *     expected!=0 要求 rc<0（不区分具体负值）。
 *   - 覆盖 ≥ 30 个关键 case：内核内存 / 内核路径 / /system/critical/
 *     ACL / 权限 0 与 1 的隔离 / PID 0-99 保护 / IPC / CONFIG。 */
void permission_selftest(void);

#endif /* OMNIBRIDGE_PERMISSION_TEST_H */