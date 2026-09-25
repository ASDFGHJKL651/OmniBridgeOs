#include "ipi.h"
#include "lapic.h"
#include "sched.h"
#include "serial.h"

/*
 * 单核阶段：IPI 仅用于验证发送/接收路径是否工作。
 * 由于只有一个 CPU，向自身发送的 IPI 会立即在返回用户态前被处理，
 * 因此 ipi_handler 中直接请求一次重调度，并打印一条日志用于观察。
 *
 * SMP 启用后，ipi_handler 应该：
 *   - 若为 IPI_VECTOR_RESCHED：设置每 CPU 的 need_resched 标志，
 *     并在当前 CPU 的中断返回路径上触发 schedule()；
 *   - 若为 IPI_VECTOR_TLB_FLUSH：执行 CR3 重载（invlpg）。
 * 这些留待步骤 31。
 */

void ipi_init(void)
{
    serial_printf("[IPI] init: resched_vec=0x%x tlb_vec=0x%x\n",
                  (unsigned)IPI_VECTOR_RESCHED,
                  (unsigned)IPI_VECTOR_TLB_FLUSH);
}

void ipi_send_resched(uint32_t apic_id)
{
    lapic_send_ipi(apic_id, IPI_VECTOR_RESCHED);
}

void ipi_send_resched_self(void)
{
    lapic_send_ipi_self(IPI_VECTOR_RESCHED);
}

void ipi_handler(uint32_t vector)
{
    switch (vector) {
    case IPI_VECTOR_RESCHED:
        /* 单核：本中断在 irq_handler 中被分派，此时已发送 EOI。
         * 直接触发一次调度即可。 */
        schedule();
        break;

    case IPI_VECTOR_TLB_FLUSH:
        /* 后续 SMP 阶段实现：重载 CR3 刷新 TLB */
        break;

    default:
        serial_printf("[IPI] unknown vector 0x%x\n", (unsigned)vector);
        break;
    }
}