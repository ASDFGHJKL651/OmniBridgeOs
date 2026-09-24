/*===OmniBridgeOs/kernel/arch/x64/audit_test.c===*/
#include "audit_test.h"
#include "audit.h"
#include "serial.h"

/*
 * 环形缓冲自检。严禁调用 kmalloc / pmm_* / task_create。
 * 注意：本测试会写满环形缓冲；运行后需重新审计关键事件已由启动序列
 *       处理（在 permission_selftest 之后运行，之前的审计会被覆盖）。
 */

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[AUDIT-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[AUDIT-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

void audit_test(void)
{
    failures = 0;

    audit_init();
    check("init: current_count == 0", audit_current_count() == 0);
    check("init: total_count == 0",   audit_total_count() == 0);

    /* 单条写入 */
    audit_event(AUDIT_EV_PID_VIOLATION, AUDIT_LVL_CRITICAL,
                100, 5, 0, 0, NULL);
    check("single: current_count == 1", audit_current_count() == 1);
    check("single: total_count == 1",   audit_total_count() == 1);

    struct audit_entry e;
    int ok = (audit_peek(0, &e) == 0);
    check("peek seq=0", ok);
    if (ok) {
        check("peek type", e.type == AUDIT_EV_PID_VIOLATION);
        check("peek pid",  e.caller_pid == 100);
        check("peek arg0", e.arg0 == 5);
    }

    /* seq 单调 */
    audit_event(AUDIT_EV_KERNEL_MEM_WRITE, AUDIT_LVL_CRITICAL,
                200, 0xFFFF800000000000ULL, 0, 0x02, NULL);
    audit_event(AUDIT_EV_KERNEL_MEM_READ, AUDIT_LVL_CRITICAL,
                201, 0xFFFF800000001000ULL, 0, 0x01, NULL);
    struct audit_entry e2, e3;
    ok  = (audit_peek(1, &e2) == 0);
    ok &= (audit_peek(2, &e3) == 0);
    if (ok) {
        check("seq monotonic (1 < 2)", e2.seq < e3.seq);
    }

    /* 写满缓冲 */
    uint64_t before = audit_total_count();
    for (uint64_t i = 0; i < AUDIT_RING_SIZE; ++i) {
        audit_event(AUDIT_EV_CRITICAL_ACCESS, AUDIT_LVL_CRITICAL,
                    1000 + i, 0, 0, 0x01, "/system/critical/x");
    }
    check("filled: current_count == RING_SIZE",
          audit_current_count() == AUDIT_RING_SIZE);
    check("filled: total increased",
          audit_total_count() == before + AUDIT_RING_SIZE);

    /* 再写一条，current_count 仍为 RING_SIZE */
    audit_event(AUDIT_EV_PID_VIOLATION, AUDIT_LVL_CRITICAL,
                9999, 5, 0, 0, NULL);
    check("overwrite: current_count stays RING_SIZE",
          audit_current_count() == AUDIT_RING_SIZE);
    check("overwrite: total increased by 1",
          audit_total_count() == before + AUDIT_RING_SIZE + 1);

    /* 最老的 seq 已被覆盖 */
    ok = (audit_peek(0, &e) != 0);
    check("overwrite: seq=0 evicted", ok);

    /* 最新的条目可读 */
    uint64_t latest = audit_total_count() - 1;
    ok = (audit_peek(latest, &e) == 0);
    check("overwrite: latest readable", ok);

    if (failures == 0) {
        serial_printf("[AUDIT-TEST] selftest OK\n");
    } else {
        serial_printf("[AUDIT-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
}
/*===OmniBridgeOs/kernel/arch/x64/audit_test.c 结束===*/