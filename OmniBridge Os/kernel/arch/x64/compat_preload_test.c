/*===OmniBridgeOs/kernel/arch/x64/compat_preload_test.c===*/
#include "compat_preload_test.h"
#include "compat_preload.h"
#include "art.h"
#include "task.h"
#include "shm.h"
#include "serial.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[COMPAT-PRELOAD-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[COMPAT-PRELOAD-TEST] FAIL: %s\n", desc);
        failures++;
    }
}

static void make_fake(struct task_t *t, uint64_t pid, uint8_t priv)
{
    uint8_t *p = (uint8_t *)t;
    for (unsigned i = 0; i < sizeof(*t); ++i) p[i] = 0;
    t->pid             = pid;
    t->privilege_level = priv;
    t->security_token.level = priv;
    t->compat_region   = 0;
}

void compat_preload_test(void)
{
    failures = 0;
    serial_printf("[COMPAT-PRELOAD-TEST] === begin ===\n");

    compat_preload_init();

    /* 1) 非 PID 1 拒绝 */
    struct task_t fake2;
    make_fake(&fake2, 2000, 9);
    int rc = compat_preload_run(&fake2);
    check("preload run as non-PID1 fails", rc == OB_EPERM);

    /* 2) PID 1 成功 */
    struct task_t init_task;
    make_fake(&init_task, 1, 9);
    rc = compat_preload_run(&init_task);
    check("preload run as PID 1 succeeds", rc == 0);

    /* 3) 幂等 */
    rc = compat_preload_run(&init_task);
    check("preload run idempotent", rc == 0);

    /* 4) ART 表已初始化 */
    check("ART table initialized", art_count() >= 0);

    /* 5) 继承 */
    struct task_t child;
    make_fake(&child, 2000, 9);
    compat_preload_inherit(&child, &init_task);
    check("inherit shares region",
          child.compat_region == init_task.compat_region &&
          child.compat_region != 0);

    /* 6) 释放引用，验证 refcount 变化 */
    if (child.compat_region && child.compat_region->shm) {
        uint32_t rc_before = shm_refcount(child.compat_region->shm);
        compat_preload_release(&child);
        uint32_t rc_after = shm_refcount(init_task.compat_region->shm);
        check("release decrements refcount",
              child.compat_region == 0 && rc_after < rc_before);
    } else {
        check("release decrements refcount", 0);
    }

    /* 7) 区域指针非空 */
    check("region pointer non-NULL",
          compat_preload_region_ptr() != 0);

    if (failures == 0) {
        serial_printf("[COMPAT-PRELOAD-TEST] selftest OK\n");
    } else {
        serial_printf("[COMPAT-PRELOAD-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[COMPAT-PRELOAD-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/compat_preload_test.c 结束===*/