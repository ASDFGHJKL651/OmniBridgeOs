/*===OmniBridgeOs/kernel/arch/x64/sandbox.c===*/
#include "sandbox.h"
#include "see.h"
#include "see_whitelist.h"
#include "vfs.h"
#include "tmpfs.h"
#include "kmalloc.h"
#include "serial.h"
#include "sched.h"
#include "task.h"
#include "permission.h"

static int sandbox_mount_path(uint64_t pid, char *out, size_t out_size)
{
    if (!out || out_size < 32) return OB_EINVAL;
    const char *pfx = "/sandbox/";
    size_t n = 0;
    while (pfx[n] && n + 1 < out_size) { out[n] = pfx[n]; ++n; }

    char num[24];
    int nd = 0;
    uint64_t v = pid;
    if (v == 0) num[nd++] = '0';
    while (v && nd < (int)sizeof(num) - 1) {
        num[nd++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = nd - 1; i >= 0; --i) {
        if (n + 1 >= out_size) return OB_EINVAL;
        out[n++] = num[i];
    }
    out[n] = '\0';
    return 0;
}

int sandbox_setup_fs(struct task_t *t)
{
    if (!t) return OB_EINVAL;
    if (t->sandbox_sb_ptr) return 0;

    int rc = vfs_mkdir("/sandbox", 0755);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[SANDBOX] mkdir /sandbox failed rc=%d\n", rc);
        return rc;
    }

    char mount_path[128];
    rc = sandbox_mount_path(t->pid, mount_path, sizeof(mount_path));
    if (rc != 0) return rc;

    rc = vfs_mkdir(mount_path, 0755);
    if (rc != 0 && rc != OB_EEXIST) {
        serial_printf("[SANDBOX] mkdir %s failed rc=%d\n", mount_path, rc);
        return rc;
    }

    struct vfs_superblock *sb = tmpfs_mount("sandbox", 512ULL * 1024 * 1024);
    if (!sb) return OB_ENOMEM;

    rc = vfs_mount(mount_path, sb);
    if (rc != 0) {
        tmpfs_umount(sb);
        serial_printf("[SANDBOX] mount %s failed rc=%d\n", mount_path, rc);
        return rc;
    }

    t->sandbox_sb_ptr = sb;
    for (size_t i = 0; mount_path[i] && i + 1 < sizeof(t->sandbox_dir_path); ++i) {
        t->sandbox_dir_path[i] = mount_path[i];
        t->sandbox_dir_path[i + 1] = '\0';
    }

    serial_printf("[SANDBOX] fs mounted at %s (pid=%llu)\n",
                  mount_path, (unsigned long long)t->pid);
    return 0;
}

void sandbox_cleanup_fs(struct task_t *t)
{
    if (!t || !t->sandbox_sb_ptr) return;

    if (t->sandbox_dir_path[0]) {
        int rc = vfs_umount(t->sandbox_dir_path);
        if (rc != 0) {
            serial_printf("[SANDBOX] WARN: umount %s rc=%d\n",
                          t->sandbox_dir_path, rc);
        }
        rc = vfs_rmdir(t->sandbox_dir_path);
        if (rc != 0 && rc != OB_ENOENT) {
            serial_printf("[SANDBOX] WARN: rmdir %s rc=%d\n",
                          t->sandbox_dir_path, rc);
        }
    }

    t->sandbox_sb_ptr = 0;
    t->sandbox_dir_path[0] = '\0';
}

static void sandbox_list_cb(struct task_t *t, void *arg)
{
    (void)arg;
    if (!(t->sandbox_flags & OBSANDBOX_ACTIVE)) return;

    serial_printf("sandbox pid=%llu priv=%u critical=%u name=%s\n",
                  (unsigned long long)t->pid,
                  (unsigned)t->privilege_level,
                  (unsigned)t->is_critical,
                  t->name ? t->name : "(null)");
}

void sandbox_list_dump(void)
{
    serial_printf("[SANDBOX] active sandboxes:\n");
    task_iterate(sandbox_list_cb, 0);
}

void sandbox_entry_trampoline(void *arg)
{
    (void)arg;
    struct task_t *self = task_from_thread(sched_current());
    if (self) {
        serial_printf("[SANDBOX] pid=%llu entered sandbox, exiting cleanly\n",
                      (unsigned long long)self->pid);
    }
    task_exit(0);
    for (;;) { __asm__ __volatile__("hlt"); }
}

struct task_t *sandbox_create_process(struct task_t *parent,
                                      const char *target_path,
                                      const char *argv_text,
                                      uint8_t level)
{
    (void)target_path;
    (void)argv_text;

    if (!parent) return 0;
    if (parent->privilege_level == 0) return 0;

    if (level == 9) level = 6;
    uint8_t child_priv = parent->privilege_level;
    if (level < child_priv) child_priv = level;

    struct task_t *t = task_create("sandbox",
                                   sandbox_entry_trampoline, 0,
                                   parent,
                                   0,
                                   child_priv,
                                   OBSANDBOX_ACTIVE,
                                   0);
    if (!t) return 0;

    t->sandbox_flags = OBSANDBOX_ACTIVE;
    t->is_critical   = 0;
    t->pending_kill  = 0;

    struct see_instance *inst = 0;
    int rc = see_create_instance(t->pid, OBSANDBOX_ACTIVE, &inst);
    if (rc != 0) {
        t->sandbox_flags = 0;
        task_destroy(t);
        return 0;
    }

    rc = see_whitelist_install_default(inst);
    if (rc != 0) {
        see_destroy_instance(inst);
        t->sandbox_flags = 0;
        task_destroy(t);
        return 0;
    }

    see_bind_task(t, inst);

    rc = sandbox_setup_fs(t);
    if (rc != 0) {
        see_destroy_instance(inst);
        t->syscall_table_ptr = 0;
        t->sandbox_flags = 0;
        task_destroy(t);
        return 0;
    }

    serial_printf("[SANDBOX] created pid=%llu parent=%llu level=%u\n",
                  (unsigned long long)t->pid,
                  (unsigned long long)parent->pid,
                  (unsigned)child_priv);
    return t;
}

int sandbox_kill(struct task_t *caller, uint64_t pid)
{
    struct task_t *target = task_find_by_pid(pid);
    if (!target) return OB_ENOENT;
    if (!(target->sandbox_flags & OBSANDBOX_ACTIVE)) return OB_EINVAL;

    if (caller && caller != target) {
        if (caller->privilege_level < 7 &&
            caller->pid != target->parent_pid) {
            audit_pid_violation(caller->pid, target->pid);
            return OB_EPERM;
        }
    }

    int rc = see_kill_sandbox(caller, target);
    if (rc != 0) return rc;

    serial_printf("[SANDBOX] kill requested pid=%llu\n",
                  (unsigned long long)pid);
    return 0;
}

int sandbox_net_is_unreachable(void)
{
    return OB_ENETUNREACH;
}