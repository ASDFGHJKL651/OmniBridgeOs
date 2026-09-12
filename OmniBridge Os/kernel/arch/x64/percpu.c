#include "percpu.h"
#include "serial.h"

/*
 * 单核启动阶段：ob_cpu_id 恒为 0。
 * SMP 启用后由 AP 引导流程调用 percpu_set_cpu_id() 更新。
 */
int ob_cpu_id = 0;

void percpu_init(void)
{
    ob_cpu_id = 0;
    serial_printf("[PERCPU] initialized, max_cpus=%d, current_cpu=%d\n",
                  OB_MAX_CPUS, ob_cpu_id);
}