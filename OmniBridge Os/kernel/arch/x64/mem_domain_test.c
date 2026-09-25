/*===OmniBridgeOs/kernel/arch/x64/mem_domain_test.c===*/
#include "mem_domain_test.h"
#include "mem_domain.h"
#include "vmm.h"
#include "pmm.h"
#include "task.h"
#include "serial.h"
#include "vfs.h"

static int failures = 0;

static void check(const char *desc, int ok)
{
    if (ok) {
        serial_printf("[MEM-DOMAIN-TEST] OK  : %s\n", desc);
    } else {
        serial_printf("[MEM-DOMAIN-TEST] FAIL: %s\n", desc);
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
}

void mem_domain_test(void)
{
    failures = 0;
    serial_printf("[MEM-DOMAIN-TEST] === begin ===\n");

    uint64_t free_before = pmm_free_pages_count();

    /* ---------- 1) 权限 0：ISOLATION_MEM ---------- */
    {
        struct task_t fake;
        make_fake(&fake, 6000, 0);

        int rc = mem_domain_create(&fake);
        check("priv0 create sets ISOLATION_MEM",
              rc == 0 && fake.isolation_mode == ISOLATION_MEM);
        check("priv0 create sets pml4",
              fake.mem_domain.pml4_self_ptr != 0);

        mem_domain_destroy(&fake);
    }

    /* ---------- 2) 权限 1：ISOLATION_TMPDIR ---------- */
    {
        struct task_t fake;
        make_fake(&fake, 6001, 1);

        int rc = mem_domain_create(&fake);
        check("priv1 create sets ISOLATION_TMPDIR",
              rc == 0 && fake.isolation_mode == ISOLATION_TMPDIR);

        mem_domain_destroy(&fake);
    }

    /* ---------- 3) contains 边界 ---------- */
    {
        struct task_t fake;
        make_fake(&fake, 6002, 0);

        int rc = mem_domain_create(&fake);
        if (rc == 0) {
            check("contains(base) == 1",
                  mem_domain_contains(&fake,
                                      fake.mem_domain.base_vaddr) == 1);
            check("contains(limit) == 0",
                  mem_domain_contains(&fake,
                                      fake.mem_domain.limit_vaddr) == 0);
            check("contains(base-1) == 0",
                  mem_domain_contains(&fake,
                                      fake.mem_domain.base_vaddr - 1) == 0);
            check("contains(limit+1) == 0",
                  mem_domain_contains(&fake,
                                      fake.mem_domain.limit_vaddr + 1) == 0);
            mem_domain_destroy(&fake);
        } else {
            check("contains(base) == 1 (create failed)", 0);
            check("contains(limit) == 0 (create failed)", 0);
            check("contains(base-1) == 0 (create failed)", 0);
            check("contains(limit+1) == 0 (create failed)", 0);
        }
    }

    /* ---------- 4) grow 超 quota ---------- */
    {
        struct task_t fake;
        make_fake(&fake, 6003, 0);

        int rc = mem_domain_create(&fake);
        if (rc == 0) {
            uint64_t cur = mem_domain_used_pages(&fake);
            uint64_t want = MEM_DOMAIN_PRIV0_MAX_PAGES + 1ULL - cur;
            rc = mem_domain_grow(&fake, want);
            check("grow exceeds quota -> -ENOMEM", rc == OB_ENOMEM);
            check("grow failure leaves state intact",
                  mem_domain_used_pages(&fake) == cur);
            mem_domain_destroy(&fake);
        } else {
            check("grow exceeds quota -> -ENOMEM (create failed)", 0);
            check("grow failure leaves state intact (create failed)", 0);
        }
    }

    /* ---------- 5) destroy 清空字段 ---------- */
    {
        struct task_t fake;
        make_fake(&fake, 6004, 0);

        int rc = mem_domain_create(&fake);
        if (rc == 0) {
            mem_domain_destroy(&fake);
            check("destroy clears pml4_self_ptr",
                  fake.mem_domain.pml4_self_ptr == 0);
            check("destroy clears phys_frame_count",
                  fake.mem_domain.phys_frame_count == 0);
            check("destroy clears isolation_mode",
                  fake.isolation_mode == ISOLATION_NONE);
        } else {
            check("destroy clears pml4_self_ptr (create failed)", 0);
            check("destroy clears phys_frame_count (create failed)", 0);
            check("destroy clears isolation_mode (create failed)", 0);
        }
    }

    /* ---------- 6) double create 被拒 ---------- */
    {
        struct task_t fake;
        make_fake(&fake, 6005, 0);

        int rc1 = mem_domain_create(&fake);
        int rc2 = mem_domain_create(&fake);
        check("double create rejected",
              rc1 == 0 && rc2 != 0);
        mem_domain_destroy(&fake);
    }

    /* ---------- 7) 无页泄漏 ---------- */
    {
        uint64_t free_after = pmm_free_pages_count();
        check("no page leak", free_after == free_before);
        if (free_after != free_before) {
            serial_printf("[MEM-DOMAIN-TEST]   before=%llu after=%llu\n",
                          (unsigned long long)free_before,
                          (unsigned long long)free_after);
        }
    }

    if (failures == 0) {
        serial_printf("[MEM-DOMAIN-TEST] selftest OK\n");
    } else {
        serial_printf("[MEM-DOMAIN-TEST] selftest FAILED: %d case(s)\n",
                      failures);
    }
    serial_printf("[MEM-DOMAIN-TEST] === end ===\n");
}
/*===OmniBridgeOs/kernel/arch/x64/mem_domain_test.c 结束===*/