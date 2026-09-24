#ifndef OMNIBRIDGE_PERCPU_H
#define OMNIBRIDGE_PERCPU_H

/*
 * 每 CPU 变量基础设施 —— 为 SMP 做准备的极简实现。
 *
 * 说明（人工必须审查）：
 *   - 当前单核启动阶段 ob_cpu_id 恒为 0；SMP 启用后由 APIC ID 映射
 *     到 0..N-1 的逻辑 CPU 编号。
 *   - 不依赖 %gs 段基址，采用「全局数组 + 逻辑 CPU 编号索引」的最简
 *     方案：无段寄存器依赖、无 GDT 修改，SMP 启用前即可编译通过。
 *   - 后续如果切换到 %gs 基址方案，可在此头文件内重定向宏定义，而
 *     不需要修改使用者代码。
 */

#include <stdint.h>

#define OB_MAX_CPUS 64

/* 当前 CPU 逻辑编号，由调度器或 BSP 初始化阶段写入 */
extern int ob_cpu_id;

static inline int percpu_cpu_id(void)
{
    return ob_cpu_id;
}

static inline void percpu_set_cpu_id(int id)
{
    ob_cpu_id = id;
}

/* 声明/定义每 CPU 数组：名称为 __percpu_<name> */
#define DEFINE_PER_CPU(type, name)  \
    type __percpu_##name[OB_MAX_CPUS]

#define DECLARE_PER_CPU(type, name) \
    extern type __percpu_##name[OB_MAX_CPUS]

/* 访问当前 CPU 的变量；返回可写左值 */
#define get_cpu_var(name)   (__percpu_##name[ob_cpu_id])
#define put_cpu_var(name)   ((void)0)

/* 访问指定 CPU 的变量 */
#define per_cpu_ptr(name, cpu) (&(__percpu_##name[(cpu)]))

/* 初始化 per-CPU 基础设施（单核时无操作） */
void percpu_init(void);

#endif /* OMNIBRIDGE_PERCPU_H */