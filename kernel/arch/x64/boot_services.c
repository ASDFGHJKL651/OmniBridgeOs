#include "boot_services.h"
#include "task.h"
#include "vfs.h"
#include "obr.h"
#include "serial.h"
#include "printk.h"

/*
 * 系统服务入口：占位实现。
 *
 * 骨架阶段的行为（人工必须审查）：
 *   1) 打印一次 banner，证明线程已被调度进入；
 *   2) 调用 task_exit(0) 主动结束线程，把 CPU 让给后续线程。
 *
 * 为什么不使用 "for(;;) hlt" 空循环：
 *   当前 sched_tick() 的语义是"每 tick 无条件递减当前线程时间片"，
 *   并不区分线程是在真正运行还是在 hlt 等待。若 4 个服务线程（全部
 *   在 HIGH 队列）进入 hlt 死循环，它们的时间片归零后会被重新放回
 *   HIGH 队列，导致 HIGH 队列永不为空，从而饿死 MID 队列的 A/B 和
 *   LOW 队列的 idle。
 *
 *   骨架阶段的替代方案是：打印一次即退出。这样 HIGH 队列被依次清空，
 *   调度器自然向下走到 MID/LOW 队列。
 *
 *   步骤 13+ 会引入真正的用户态 Init，届时会替换为 "服务主循环 +
 *   阻塞/唤醒" 模型；那个模型需要在调度器层面支持 BLOCKED 状态与
 *   唤醒源，不属于本步范围。
 */
static void service_entry(void *arg)
{
    (void)arg;
    serial_printf("[SVC] service thread entered (hlt loop)\n");
    task_exit(0);
    for (;;) { __asm__ __volatile__("hlt"); }   /* not reached */
}

static int is_critical_path(const char *path)
{
    const char *pfx = "/system/critical/";
    for (int i = 0; pfx[i]; ++i) {
        if (path[i] != pfx[i]) return 0;
    }
    return 1;
}

/* 尝试读取 /system/critical/ 下服务的 .obr 头并验证。
 * 文件不存在时打印 note（不算失败）。 */
static void try_verify_obr(const char *path)
{
    struct vfs_file *f = 0;
    int rc = vfs_open(path, VFS_O_RDONLY, &f);
    if (rc != 0 || !f) {
        serial_printf("[BOOT]   note: '%s' not present (rc=%d)\n",
                      path, rc);
        return;
    }

    uint8_t hbuf[128];
    int64_t nr = vfs_read(f, hbuf, sizeof(hbuf));
    if (nr >= (int64_t)sizeof(struct obr_header)) {
        if (obr_validate_header((const struct obr_header *)hbuf,
                                 (uint64_t)nr) == 0) {
            const struct obr_header *h = (const struct obr_header *)hbuf;
            serial_printf("[BOOT]   '%s' entry=0x%llx\n",
                          path, (unsigned long long)h->entry_point);
        } else {
            serial_printf("[BOOT]   WARN: '%s' header invalid\n", path);
        }
    } else {
        serial_printf("[BOOT]   WARN: '%s' too small (%lld bytes)\n",
                      path, (long long)nr);
    }
    vfs_close(f);
}

int boot_services_start(struct obinit_config *cfg)
{
    if (!cfg) return -1;

    struct obinit_service *svc = cfg->head;
    while (svc) {
        struct task_t *t = task_create(svc->name, service_entry, 0,
                                       0, /* parent = bootstrap */
                                       svc->pid_hint,
                                       svc->privilege,
                                       0, /* sandbox_flags */
                                       svc->critical);
        if (!t) {
            serial_printf("[BOOT] failed to create service '%s'\n",
                          svc->name);
            return -1;
        }

        if (svc->path[0] != '\0' && is_critical_path(svc->path)) {
            try_verify_obr(svc->path);
        }

        serial_printf("[BOOT] service '%s' pid=%llu priv=%u critical=%u\n",
                      svc->name,
                      (unsigned long long)t->pid,
                      (unsigned)svc->privilege,
                      (unsigned)t->is_critical);

        svc = svc->next;
    }
    return 0;
}

int boot_services_default(void)
{
    static const struct {
        const char *name;
        const char *path;
        uint8_t     priv;
        uint64_t    pid;
        int         critical;
    } svcs[] = {
        { "init",        "/system/critical/init.obr",         8, 1, 1 },
        { "secmgr",      "/system/critical/secmgr.obr",       8, 2, 1 },
        { "servicehost", "/system/critical/servicehost.obr",  7, 3, 1 },
        { "auditd",      "/system/critical/auditd.obr",       8, 4, 1 },
    };
    const uint32_t n = (uint32_t)(sizeof(svcs) / sizeof(svcs[0]));

    for (uint32_t i = 0; i < n; ++i) {
        struct task_t *t = task_create(svcs[i].name, service_entry, 0,
                                       0, svcs[i].pid, svcs[i].priv,
                                       0, svcs[i].critical);
        if (!t) {
            serial_printf("[BOOT] failed to create '%s'\n", svcs[i].name);
            return -1;
        }

        try_verify_obr(svcs[i].path);

        serial_printf("[BOOT] service '%s' pid=%llu priv=%u critical=%u\n",
                      svcs[i].name,
                      (unsigned long long)t->pid,
                      (unsigned)svcs[i].priv,
                      (unsigned)t->is_critical);
    }
    return 0;
}