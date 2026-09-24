/*===OmniBridgeOs/kernel/arch/x64/compat_preload.c===*/
#include "compat_preload.h"
#include "art.h"
#include "shm.h"
#include "kmalloc.h"
#include "serial.h"
#include "spinlock.h"
/* ★ 第 18 步 */
#include "compat_env.h"

static struct compat_preload_region *g_region = 0;
static spinlock_t g_preload_lock = SPINLOCK_INIT;
static int g_inited = 0;

void compat_preload_init(void)
{
    if (g_inited) return;
    spin_lock_init(&g_preload_lock);
    g_region = 0;
    g_inited = 1;
    serial_printf("[COMPAT-PRELOAD] init: region_size=%llu\n",
                  (unsigned long long)COMPAT_REGION_SIZE);
}

int compat_preload_run(struct task_t *caller)
{
    if (!caller) return OB_EPERM;
    if (caller->pid != 1) return OB_EPERM;
    if (caller->privilege_level < 8) return OB_EPERM;

    if (!g_inited) compat_preload_init();

    uint64_t irqf;
    spin_lock_irqsave(&g_preload_lock, &irqf);

    if (g_region) {
        spin_unlock_irqrestore(&g_preload_lock, irqf);
        serial_printf("[COMPAT-PRELOAD] already initialized, "
                      "entries=%u\n", (unsigned)art_count());
        if (!caller->compat_region) {
            caller->compat_region = g_region;
            if (g_region->shm) shm_retain(g_region->shm);
        }
        return 0;
    }

    struct compat_preload_region *r =
        (struct compat_preload_region *)kzalloc(sizeof(*r));
    if (!r) {
        spin_unlock_irqrestore(&g_preload_lock, irqf);
        return OB_ENOMEM;
    }

    struct shm_region *shm = shm_create(COMPAT_REGION_SIZE);
    if (!shm) {
        kfree(r);
        spin_unlock_irqrestore(&g_preload_lock, irqf);
        return OB_ENOMEM;
    }

    void *base = shm_addr(shm);

    r->shm            = shm;
    r->size           = COMPAT_REGION_SIZE;
    r->refcount       = 1;

    /* ★ 第 18 步：填充 PEB / auxv 模板 */
    compat_env_init();

    r->peb_template   = base;
    r->auxv_template  = (void *)((uintptr_t)base +
                                 sizeof(struct ob_peb_template));

    /* 校验区域空间足够 */
    if (sizeof(struct ob_peb_template) +
        sizeof(struct ob_auxv_template) > COMPAT_REGION_SIZE) {
        shm_release(shm);
        kfree(r);
        spin_unlock_irqrestore(&g_preload_lock, irqf);
        return OB_EINVAL;
    }

    compat_env_fill_peb((struct ob_peb_template *)r->peb_template,
                        0x400000ULL,  /* 占位镜像基址 */
                        0x70000000ULL);/* 占位堆地址 */

    compat_env_fill_auxv((struct ob_auxv_template *)r->auxv_template,
                         4096u, 0u);

    g_region = r;
    caller->compat_region = r;

    spin_unlock_irqrestore(&g_preload_lock, irqf);

    /* 初始化 ART 表（幂等） */
    art_init();
    art_register("ob_test_api",    0, 0);
    art_register("ob_compat_init", 0, 0);

    /* ★ 第 18 步：把 ART 表地址写入区域 */
    {
        uint64_t irqf2;
        spin_lock_irqsave(&g_preload_lock, &irqf2);
        g_region->art_table = (void *)(uintptr_t)art_base_addr();
        spin_unlock_irqrestore(&g_preload_lock, irqf2);
    }

    serial_printf("[COMPAT-PRELOAD] PEB at %p auxv at %p ART at 0x%llx\n",
                  r->peb_template,
                  r->auxv_template,
                  (unsigned long long)art_base_addr());
    serial_printf("[COMPAT-PRELOAD] ART entries=%u region=%p size=%llu\n",
                  (unsigned)art_count(),
                  (void *)r->peb_template,
                  (unsigned long long)r->size);
    return 0;
}

void compat_preload_inherit(struct task_t *child, struct task_t *parent)
{
    if (!child || !parent) return;

    if (!parent->compat_region) {
        child->compat_region = 0;
        return;
    }

    child->compat_region = parent->compat_region;
    if (child->compat_region->shm) {
        shm_retain(child->compat_region->shm);
    }
}

void compat_preload_release(struct task_t *t)
{
    if (!t) return;
    if (!t->compat_region) return;

    struct compat_preload_region *r = t->compat_region;
    t->compat_region = 0;

    if (r->shm) {
        shm_release(r->shm);
    }
}

void *compat_preload_region_ptr(void)
{
    return g_region ? g_region->peb_template : 0;
}