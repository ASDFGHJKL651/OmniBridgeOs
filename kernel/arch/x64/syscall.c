/*===OmniBridgeOs/kernel/arch/x64/syscall.c===*/
#include "syscall.h"
#include "printk.h"
#include "serial.h"
#include "sched.h"
#include "task.h"
#include "permission.h"
#include "vmm.h"
#include "see.h"
#include "vfs.h"
#include "kmalloc.h"

#include "ita_sign.h"
#include "ed25519.h"
#include "uel.h"
#include "compat_preload.h"
#include "audit.h"
#include "net/net.h"
#include "net/socket.h"
#include "net/netns.h"
#include "user/user.h"
#include "user/signal.h"
#include "user/tty.h"
#include "pmm.h"
#include "futex.h"
#include "seccomp.h"

extern void syscall_entry(void);

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_FMASK  0xC0000084u

#define EFER_SCE   (1ULL << 0)

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr"
                         :: "c"(msr), "a"(lo), "d"(hi)
                         : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr"
                         : "=a"(lo), "=d"(hi)
                         : "c"(msr)
                         : "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void syscall_init(void)
{
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    uint64_t star = ((uint64_t)0x13u << 48) | ((uint64_t)0x08u << 32);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x47700ULL);

    serial_printf("[SYSCALL] EFER.SCE=1 STAR=0x%llx LSTAR=0x%llx FMASK=0x47700\n",
                  (unsigned long long)star,
                  (unsigned long long)(uintptr_t)syscall_entry);
}

#define VFS_MAX_FDS 64

static int fd_alloc(struct task_t *cur)
{
    if (!cur) return -1;
    for (int i = 3; i < VFS_MAX_FDS; ++i) {
        if (!cur->fd_table[i]) return i;
    }
    return -1;
}

static struct vfs_file *fd_get(struct task_t *cur, int fd)
{
    if (!cur) return 0;
    if (fd < 0 || fd >= VFS_MAX_FDS) return 0;
    return cur->fd_table[fd];
}

static void fd_release(struct task_t *cur, int fd)
{
    if (!cur) return;
    if (fd < 0 || fd >= VFS_MAX_FDS) return;
    cur->fd_table[fd] = 0;
}

static inline struct task_t *current_task(void)
{
    struct thread *th = sched_current();
    if (!th) return NULL;
    return (struct task_t *)th->task;
}

static void log_dispatch(struct task_t *cur, uint64_t nr)
{
    serial_printf("[SYSCALL] nr=0x%llx pid=%llu priv=%u critical=%u sandbox=0x%x\n",
                  (unsigned long long)nr,
                  (unsigned long long)(cur ? cur->pid : 0),
                  (unsigned)(cur ? cur->privilege_level : 0),
                  (unsigned)(cur ? cur->is_critical : 0),
                  (unsigned)(cur ? cur->sandbox_flags : 0));
}

static int read_whole_file(const char *path,
                           uint8_t **out_buf, uint64_t *out_len)
{
    struct vfs_file *f = 0;
    int rc = vfs_open(path, VFS_O_RDONLY, &f);
    if (rc != 0 || !f) return rc ? rc : OB_ENOENT;

    uint64_t cap   = 4096;
    uint64_t total = 0;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)cap);
    if (!buf) { vfs_close(f); return OB_ENOMEM; }

    for (;;) {
        if (total >= UEL_MAX_IMAGE_SIZE) {
            kfree(buf); vfs_close(f);
            return OB_EINVAL;
        }
        if (total >= cap) {
            uint64_t ncap = cap * 2;
            if (ncap > UEL_MAX_IMAGE_SIZE) ncap = UEL_MAX_IMAGE_SIZE;
            uint8_t *nb = (uint8_t *)kmalloc((size_t)ncap);
            if (!nb) { kfree(buf); vfs_close(f); return OB_ENOMEM; }
            for (uint64_t i = 0; i < total; ++i) nb[i] = buf[i];
            kfree(buf);
            buf = nb;
            cap = ncap;
        }
        int64_t n = vfs_read(f, buf + total, cap - total);
        if (n < 0) { kfree(buf); vfs_close(f); return (int)n; }
        if (n == 0) break;
        total += (uint64_t)n;
    }
    vfs_close(f);

    *out_buf = buf;
    *out_len = total;
    return 0;
}

/*
 * ★★★ 修复 2：user_map_pages 接入内存配额 ★★★
 *
 * 人工必须审查：
 *   - 单位是页（4KB）。
 *   - 配额 0 表示不限制。
 *   - 超限返回 -ENOMEM，不 panic。
 *   - mem_pages_used 只统计用户页，不含内核栈/页表页。
 *   - 失败回滚时不应把已计入的 mem_pages_used 泄漏；本函数在失败时
 *     立即返回，只有成功的分配才累加。
 */
static int user_map_pages(struct task_t *t, uint64_t va, uint64_t npages,
                          uint64_t flags)
{
    if (!t) return OB_EPERM;

    if (!user_range_ok(va, npages * PAGE_SIZE)) return OB_EFAULT;

    /* ★ 修复 2：配额检查（在映射任何页之前） */
    if (t->mem_quota_pages > 0) {
        uint64_t after = (uint64_t)t->mem_pages_used + npages;
        if (after > t->mem_quota_pages) {
            serial_printf("[QUOTA] pid=%llu mem quota exceeded: "
                          "%llu + %llu > %u\n",
                          (unsigned long long)t->pid,
                          (unsigned long long)t->mem_pages_used,
                          (unsigned long long)npages,
                          (unsigned)t->mem_quota_pages);
            return OB_ENOMEM;
        }
    }

    struct user_ctx *uc = user_get_ctx(t);
    uint64_t *target_pml4 = 0;
    if (t->priv_iso_ready && t->mem_domain.pml4_self_ptr) {
        target_pml4 = t->mem_domain.pml4_self_ptr;
    } else if (uc && uc->pml4) {
        target_pml4 = uc->pml4;
    } else {
        return OB_EPERM;
    }

    for (uint64_t i = 0; i < npages; ++i) {
        struct page *pg = pmm_alloc_pages(0);
        if (!pg) return OB_ENOMEM;
        uint64_t pa = page_to_phys(pg);
        uint64_t pte_flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        if (!(flags & 0x04u)) pte_flags |= PTE_NX;
        int rc = vmm_map_page(target_pml4, va + i * PAGE_SIZE, pa, pte_flags);
        if (rc != 0) {
            pmm_free_pages(pg, 0);
            return OB_EIO;
        }
    }

    /* ★ 修复 2：配额累加（仅在全部页成功映射后） */
    t->mem_pages_used += (uint32_t)npages;

    return 0;
}

int64_t syscall_dispatcher(struct syscall_frame *f)
{
    serial_printf("[SYSCALL-ENTRY] rax=0x%llx rdi=0x%llx "
                  "rsi=0x%llx rdx=0x%llx r10=0x%llx\n",
                  (unsigned long long)(f ? f->rax : 0),
                  (unsigned long long)(f ? f->rdi : 0),
                  (unsigned long long)(f ? f->rsi : 0),
                  (unsigned long long)(f ? f->rdx : 0),
                  (unsigned long long)(f ? f->r10 : 0));

    struct task_t *cur = current_task();
    uint64_t nr = f->rax;

    if (!cur) {
        serial_printf("[SYSCALL] nr=0x%llx no-task, denied\n",
                      (unsigned long long)nr);
        return OB_EPERM;
    }

    log_dispatch(cur, nr);

    if (cur->seccomp_mode == SECCOMP_MODE_FILTER) {
        int src = seccomp_check(cur, nr);
        if (src != 0) return src;
    }

    if (cur->sandbox_flags & OBSANDBOX_ACTIVE) {
        int64_t rc = see_syscall_interceptor(f, cur);
        if (cur->pending_kill) {
            cur->pending_kill = 0;
            task_exit(cur->exit_code ? cur->exit_code : -1);
        }
        return rc;
    }

    switch (nr) {
    case SYS_OB_CreateProcess: {
        const char *path = (const char *)(uintptr_t)f->rdi;
        uint64_t    req  = f->r10;
        (void)req;

        if (cur->privilege_level < 2) {
            audit_critical_access(cur->pid, path ? path : "(create)",
                                  OB_ACCESS_WRITE);
            return OB_EPERM;
        }
        if (cur->children_count >= cur->child_process_limit) {
            return OB_EAGAIN;
        }
        if (path) {
            int rc = check_permission(cur, OB_RES_FILE, 0,
                                      OB_ACCESS_READ, path);
            if (rc != 0) return rc;
        }
        serial_printf("[SYSCALL] OB_CreateProcess pid=%llu path=%s (stub ENOSYS)\n",
                      (unsigned long long)cur->pid,
                      path ? path : "(null)");
        return OB_ENOSYS;
    }

    case SYS_OB_TerminateProcess: {
        uint64_t target_pid = f->rdi;
        int rc = check_permission(cur, OB_RES_PROCESS, target_pid,
                                  OB_ACCESS_WRITE, NULL);
        if (rc != 0) return rc;
        return OB_ENOSYS;
    }

    case SYS_OB_GetProcessInfo: {
        uint64_t target_pid = f->rdi;
        int rc = check_permission(cur, OB_RES_PROCESS, target_pid,
                                  OB_ACCESS_READ, NULL);
        if (rc != 0) return rc;
        return OB_ENOSYS;
    }

    case SYS_OB_SendSignal: {
        uint64_t target_pid = f->rdi;
        int rc = check_permission(cur, OB_RES_PROCESS, target_pid,
                                  OB_ACCESS_WRITE, NULL);
        if (rc != 0) return rc;
        return OB_ENOSYS;
    }

    case SYS_OB_GetCurrentToken:
        return (int64_t)(uintptr_t)&cur->security_token;

    case SYS_OB_CheckAccess: {
        int      res_type = (int)f->rdi;
        uint64_t res_id   = f->rsi;
        uint32_t mode     = (uint32_t)f->rdx;
        const char *path  = (const char *)(uintptr_t)f->r10;

        int rc = check_permission(cur, res_type, res_id, mode, path);
        return (rc == 0) ? OB_PERM_ALLOWED : OB_PERM_DENIED;
    }

    case SYS_OB_CheckAccessNative: {
        int      res_type = (int)f->rdi;
        uint64_t res_id   = f->rsi;
        uint32_t mode     = (uint32_t)f->rdx;
        const char *path  = (const char *)(uintptr_t)f->r10;

        int rc = check_permission(cur, res_type, res_id, mode, path);
        if (rc != 0) {
            audit_event(AUDIT_EV_COMPAT_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                        cur->pid, (uint64_t)res_type, res_id, mode,
                        path ? path : "(compat)");
        }
        return (rc == 0) ? OB_PERM_ALLOWED : OB_PERM_DENIED;
    }

    case SYS_OB_OpenFile: {
        const char *path = (const char *)(uintptr_t)f->rdi;
        uint64_t flags_in = f->rsi;

        if (!path) return OB_EINVAL;

        const uint64_t U_O_ACCMODE = 0x0003;
        const uint64_t U_O_RDONLY  = 0x0001;
        const uint64_t U_O_WRONLY  = 0x0002;
        const uint64_t U_O_RDWR    = 0x0003;
        const uint64_t U_O_CREAT   = 0x0010;
        const uint64_t U_O_EXCL    = 0x0020;
        const uint64_t U_O_TRUNC   = 0x0040;
        const uint64_t U_O_APPEND  = 0x0080;

        uint32_t mode      = 0;
        uint32_t vfs_flags = 0;

        uint64_t acc = flags_in & U_O_ACCMODE;
        if (acc == U_O_WRONLY) {
            mode      = OB_ACCESS_WRITE;
            vfs_flags = VFS_O_WRONLY;
        } else if (acc == U_O_RDWR) {
            mode      = OB_ACCESS_READ | OB_ACCESS_WRITE;
            vfs_flags = VFS_O_RDWR;
        } else {
            mode      = OB_ACCESS_READ;
            vfs_flags = VFS_O_RDONLY;
        }

        if (flags_in & U_O_CREAT)  vfs_flags |= VFS_O_CREAT;
        if (flags_in & U_O_EXCL)   vfs_flags |= VFS_O_EXCL;
        if (flags_in & U_O_TRUNC)  vfs_flags |= VFS_O_TRUNC;
        if (flags_in & U_O_APPEND) vfs_flags |= VFS_O_APPEND;

        int rc = check_permission(cur, OB_RES_FILE, 0, mode, path);
        if (rc != 0) return rc;

        struct vfs_file *vf = 0;
        rc = vfs_open(path, vfs_flags, &vf);
        if (rc != 0) return rc;

        int fd = fd_alloc(cur);
        if (fd < 0) {
            vfs_close(vf);
            return OB_EAGAIN;
        }
        cur->fd_table[fd] = vf;
        return (int64_t)fd;
    }

    case SYS_OB_ReadFile: {
        int fd = (int)f->rdi;
        uint64_t buf = f->rsi;
        uint64_t len = f->rdx;

        if (fd == 0) return 0;

        struct vfs_file *vf = fd_get(cur, fd);
        if (!vf) return OB_EBADF;

        int rc = check_permission(cur, OB_RES_MEMORY, buf,
                                  OB_ACCESS_WRITE, NULL);
        if (rc != 0) return rc;

        int64_t n = vfs_read(vf, (void *)(uintptr_t)buf, len);
        return n;
    }

    case SYS_OB_WriteFile: {
        int fd = (int)f->rdi;
        uint64_t buf = f->rsi;
        uint64_t len = f->rdx;

        if (fd == 1 || fd == 2) {
            int rc = check_permission(cur, OB_RES_MEMORY, buf,
                                      OB_ACCESS_READ, NULL);
            if (rc != 0) return rc;
            const char *p = (const char *)(uintptr_t)buf;
            for (uint64_t i = 0; i < len; ++i) {
                serial_putc(p[i]);
            }
            return (int64_t)len;
        }

        struct vfs_file *vf = fd_get(cur, fd);
        if (!vf) return OB_EBADF;

        int rc = check_permission(cur, OB_RES_MEMORY, buf,
                                  OB_ACCESS_READ, NULL);
        if (rc != 0) return rc;

        int64_t n = vfs_write(vf, (const void *)(uintptr_t)buf, len);
        return n;
    }

    case SYS_OB_CloseHandle: {
        int fd = (int)f->rdi;
        if (fd >= 0 && fd <= 2) return 0;

        struct vfs_file *vf = fd_get(cur, fd);
        if (!vf) return OB_EBADF;
        vfs_close(vf);
        fd_release(cur, fd);
        return 0;
    }

    case SYS_OB_VirtualAlloc: {
        uint64_t size  = f->rdi;
        uint64_t hint  = f->rsi;
        uint64_t flags = f->rdx;
        if (size == 0) return OB_EINVAL;
        uint64_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (npages > 256) return OB_EINVAL;

        if (hint == 0) {
            struct user_ctx *c = user_get_ctx(cur);
            if (!c) return OB_EPERM;
            hint = c->heap_cur;
            c->heap_cur += npages * PAGE_SIZE;
            if (c->heap_cur > c->heap_end) return OB_ENOMEM;
        }
        int rc = user_map_pages(cur, hint, npages, flags);
        if (rc != 0) return rc;
        return (int64_t)hint;
    }

    case SYS_OB_VirtualFree:
        return 0;

    case SYS_OB_LoadDriver: {
        const char *path = (const char *)(uintptr_t)f->rdi;
        int rc = check_permission(cur, OB_RES_FILE, 0,
                                  OB_ACCESS_READ | OB_ACCESS_EXEC, path);
        if (rc != 0) return rc;
        if (cur->privilege_level < 7) return OB_EPERM;
        return OB_ENOSYS;
    }

    case SYS_OB_LoadDriverSandboxed: {
        const char *path = (const char *)(uintptr_t)f->rdi;
        int rc = check_permission(cur, OB_RES_FILE, 0,
                                  OB_ACCESS_READ, path);
        if (rc != 0) return rc;
        return OB_ENOSYS;
    }

    case SYS_OB_RegisterInterrupt:
        if (cur->privilege_level < 7) return OB_EPERM;
        return OB_ENOSYS;

    case SYS_OB_CreateSandboxProcess: {
        const char *path = (const char *)(uintptr_t)f->rdi;

        if (cur->privilege_level < 2) {
            audit_critical_access(cur->pid, path ? path : "(sandbox)",
                                  OB_ACCESS_WRITE);
            return OB_EPERM;
        }
        if (cur->children_count >= cur->child_process_limit) {
            return OB_EAGAIN;
        }
        if (path) {
            int rc = check_permission(cur, OB_RES_FILE, 0,
                                      OB_ACCESS_READ, path);
            if (rc != 0) return rc;
        }
        return OB_ENOSYS;
    }

    case SYS_OB_Compat_Preload:
        if (cur->pid != 1) return OB_EPERM;
        return compat_preload_run(cur);

    case SYS_OB_ReadKernelMemory: {
        uint64_t addr = f->rdi;
        uint64_t buf  = f->rsi;
        (void)buf;

        if (!(cur->privilege_level == 9 && cur->ui_token_valid)) {
            audit_kernel_mem_violation(cur->pid, addr, OB_ACCESS_READ);
            return OB_EPERM;
        }
        return OB_ENOSYS;
    }

    case SYS_OB_WriteKernelMemory: {
        uint64_t addr = f->rdi;
        uint64_t buf  = f->rsi;
        (void)buf;

        if (!(cur->privilege_level == 9 && cur->ui_token_valid)) {
            audit_kernel_mem_violation(cur->pid, addr, OB_ACCESS_WRITE);
            return OB_EPERM;
        }
        return OB_ENOSYS;
    }

    case SYS_OB_ReadKernelFile: {
        const char *path = (const char *)(uintptr_t)f->rdi;
        uint64_t buf = f->rsi;
        (void)buf;

        if (!(cur->privilege_level == 9 && cur->ui_token_valid)) {
            audit_critical_access(cur->pid,
                                  path ? path : "(null)",
                                  OB_ACCESS_READ);
            return OB_EPERM;
        }
        return OB_ENOSYS;
    }

    case SYS_OB_InternalSign: {
        const char *path = (const char *)(uintptr_t)f->rdi;
        uint64_t out_buf = f->rsi;

        if (!(cur->privilege_level == 9 && cur->ui_token_valid)) {
            audit_critical_access(cur->pid,
                                  path ? path : "(internal-sign)",
                                  OB_ACCESS_WRITE);
            return OB_EPERM;
        }
        if (!path || !out_buf) return OB_EINVAL;

        if (!ita_sign_ready()) {
            audit_critical_access(cur->pid, path, OB_ACCESS_WRITE);
            return OB_ENOSYS;
        }

        uint8_t *file_buf = 0;
        uint64_t file_len = 0;
        int rc = read_whole_file(path, &file_buf, &file_len);
        if (rc != 0 || !file_buf) {
            audit_critical_access(cur->pid, path, OB_ACCESS_READ);
            return rc ? rc : OB_EIO;
        }

        uint8_t sig[ED25519_SIG_LEN];
        rc = ita_sign_hash(file_buf, file_len, sig);
        kfree(file_buf);

        if (rc != 0) {
            audit_critical_access(cur->pid, path, OB_ACCESS_WRITE);
            return OB_EIO;
        }

        uint8_t *dst = (uint8_t *)(uintptr_t)out_buf;
        for (int i = 0; i < ED25519_SIG_LEN; ++i) dst[i] = sig[i];

        serial_printf("[SYSCALL] OB_InternalSign pid=%llu path=%s "
                      "signed %llu bytes\n",
                      (unsigned long long)cur->pid, path,
                      (unsigned long long)file_len);
        return 0;
    }

    case SYS_OB_Socket: {
        int domain = (int)f->rdi;
        int type   = (int)f->rsi;
        int proto  = (int)f->rdx;

        if (cur->sandbox_flags != 0) {
            audit_event(AUDIT_EV_NET_SANDBOX_BLOCK, AUDIT_LVL_CRITICAL,
                        cur->pid, (uint64_t)domain, (uint64_t)type, 0,
                        "(socket)");
            return OB_ENETUNREACH;
        }

        int rc = check_permission(cur, OB_RES_IPC, 0,
                                  OB_ACCESS_READ | OB_ACCESS_WRITE,
                                  "(socket)");
        if (rc != 0) {
            audit_event(AUDIT_EV_NET_DENIED, AUDIT_LVL_CRITICAL,
                        cur->pid, (uint64_t)domain, (uint64_t)type, 0,
                        "(socket)");
            return rc;
        }
        return net_socket(domain, type, proto, cur);
    }

    case SYS_OB_Bind: {
        int fd = (int)f->rdi;
        uint32_t ip   = (uint32_t)f->rsi;
        uint16_t port = (uint16_t)f->rdx;

        if (cur->sandbox_flags != 0) {
            audit_event(AUDIT_EV_NET_SANDBOX_BLOCK, AUDIT_LVL_CRITICAL,
                        cur->pid, ip, port, 0, "(bind)");
            return OB_ENETUNREACH;
        }
        int rc = check_permission(cur, OB_RES_IPC, 0,
                                  OB_ACCESS_WRITE, "(bind)");
        if (rc != 0) return rc;
        return net_bind(fd, ip, port, cur);
    }

    case SYS_OB_Listen: {
        int fd = (int)f->rdi;
        int backlog = (int)f->rsi;
        if (cur->sandbox_flags != 0) return OB_ENETUNREACH;
        int rc = check_permission(cur, OB_RES_IPC, 0,
                                  OB_ACCESS_WRITE, "(listen)");
        if (rc != 0) return rc;
        return net_listen(fd, backlog, cur);
    }

    case SYS_OB_Connect: {
        int fd = (int)f->rdi;
        uint32_t ip   = (uint32_t)f->rsi;
        uint16_t port = (uint16_t)f->rdx;

        if (cur->sandbox_flags != 0) {
            audit_event(AUDIT_EV_NET_SANDBOX_BLOCK, AUDIT_LVL_CRITICAL,
                        cur->pid, ip, port, 0, "(connect)");
            return OB_ENETUNREACH;
        }
        int rc = check_permission(cur, OB_RES_IPC, 0,
                                  OB_ACCESS_WRITE, "(connect)");
        if (rc != 0) return rc;
        return net_connect(fd, ip, port, cur);
    }

    case SYS_OB_Send: {
        int fd = (int)f->rdi;
        const void *buf = (const void *)(uintptr_t)f->rsi;
        uint32_t len = (uint32_t)f->rdx;

        if (cur->sandbox_flags != 0) {
            audit_event(AUDIT_EV_NET_SANDBOX_BLOCK, AUDIT_LVL_CRITICAL,
                        cur->pid, (uint64_t)(uintptr_t)buf, len, 0, "(send)");
            return OB_ENETUNREACH;
        }
        int rc = check_permission(cur, OB_RES_MEMORY,
                                  (uint64_t)(uintptr_t)buf,
                                  OB_ACCESS_READ, 0);
        if (rc != 0) return rc;
        return net_send(fd, buf, len, cur);
    }

    case SYS_OB_Recv: {
        int fd = (int)f->rdi;
        void *buf = (void *)(uintptr_t)f->rsi;
        uint32_t len = (uint32_t)f->rdx;

        if (cur->sandbox_flags != 0) return OB_ENETUNREACH;
        int rc = check_permission(cur, OB_RES_MEMORY,
                                  (uint64_t)(uintptr_t)buf,
                                  OB_ACCESS_WRITE, 0);
        if (rc != 0) return rc;
        return net_recv(fd, buf, len, cur);
    }

    case SYS_OB_CloseSocket:
        return net_close((int)f->rdi, cur);

    case SYS_OB_UserExit:
        task_exit((int)f->rdi);
        return 0;

    case SYS_OB_ThreadCreate: {
        uint64_t entry = f->rdi;

        if (!user_range_ok(entry, 1)) return OB_EFAULT;

        serial_printf("[SYSCALL] ThreadCreate pid=%llu entry=0x%llx\n",
                      (unsigned long long)cur->pid,
                      (unsigned long long)entry);

        cur->futex_wake++;
        return (int64_t)(cur->pid + cur->futex_wake);
    }

    case SYS_OB_ThreadJoin:
        return 0;

    case SYS_OB_SigReturn: {
        signal_fixup_resume(cur);
        user_resume_ctx(cur, signal_resume_slot(cur));
        return 0;
    }

    case SYS_OB_FutexWait: {
        uint64_t uaddr    = f->rdi;
        uint64_t expected = f->rsi;
        uint64_t timeout  = f->rdx;

        if (!user_range_ok(uaddr, 4)) return OB_EFAULT;
        return futex_wait(uaddr, (uint32_t)expected, timeout);
    }

    case SYS_OB_FutexWake: {
        uint64_t uaddr = f->rdi;
        uint32_t count = (uint32_t)f->rsi;

        if (!user_range_ok(uaddr, 4)) return OB_EFAULT;
        return futex_wake(uaddr, count);
    }

    case SYS_OB_TcSetpgrp:
        return sys_tcsetpgrp(f->rdi);

    case SYS_OB_TcGetpgrp:
        return sys_tcgetpgrp();

    case SYS_OB_SetFsBase:
        user_set_fsbase(f->rdi);
        return 0;

    /* ★★★ 修复 8：pidns 相对化 ★★★ */
    case SYS_OB_Getpid: {
        extern uint64_t pidns_to_local(struct pid_namespace *ns,
                                        uint64_t global_pid);
        if (cur->pidns) {
            return (int64_t)pidns_to_local(cur->pidns, cur->pid);
        }
        return (int64_t)cur->pid;
    }

    case SYS_OB_Getppid: {
        extern uint64_t pidns_to_local(struct pid_namespace *ns,
                                        uint64_t global_pid);
        if (cur->pidns) {
            return (int64_t)pidns_to_local(cur->pidns, cur->parent_pid);
        }
        return (int64_t)cur->parent_pid;
    }

    case SYS_OB_Sleep: {
        uint64_t secs = f->rdi;
        if (secs > 60) secs = 60;

        uint64_t ticks = secs * 100;
        if (ticks == 0) ticks = 1;

        __asm__ __volatile__("sti" ::: "memory");
        for (uint64_t i = 0; i < ticks; ++i) {
            __asm__ __volatile__("hlt" ::: "memory");
        }
        __asm__ __volatile__("cli" ::: "memory");

        return 0;
    }

    case SYS_OB_GetTime: {
        uint32_t lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t tsc = ((uint64_t)hi << 32) | lo;
        return (int64_t)(tsc / 1000000000ULL);
    }

    case SYS_OB_Brk: {
        uint64_t want = f->rdi;
        struct user_ctx *c = user_get_ctx(cur);
        if (!c) return OB_EPERM;
        if (want == 0) return (int64_t)c->heap_cur;

        if (want > c->heap_end) return OB_ENOMEM;

        uint64_t want_aligned =
            (want + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

        if (want_aligned <= c->heap_cur) {
            return (int64_t)c->heap_cur;
        }

        uint64_t npages = (want_aligned - c->heap_cur) / PAGE_SIZE;
        if (npages == 0) return (int64_t)c->heap_cur;

        int rc = user_map_pages(cur, c->heap_cur, npages, 0);
        if (rc != 0) return rc;

        uint64_t old = c->heap_cur;
        c->heap_cur = want_aligned;

        serial_printf("[BRK] pid=%llu heap: 0x%llx -> 0x%llx (+%llu pages)\n",
                      (unsigned long long)cur->pid,
                      (unsigned long long)old,
                      (unsigned long long)c->heap_cur,
                      (unsigned long long)npages);
        return (int64_t)old;
    }

    case SYS_OB_Sigaction:
        return sys_signal((int)f->rdi, (ob_sighandler_t)f->rsi);

    case SYS_OB_Kill:
        return sys_kill(f->rdi, (int)f->rsi);

    case SYS_OB_ThreadSpawn: {
        uint64_t entry      = f->rdi;
        uint64_t arg        = f->rsi;
        uint64_t ustack_top = f->rdx;

        if (!user_range_ok(entry, 1)) return OB_EFAULT;
        if (ustack_top < 64) return OB_EFAULT;
        if (!user_range_ok(ustack_top - 64, 64)) return OB_EFAULT;

        struct user_ctx *parent_uc = user_get_ctx(cur);
        if (!parent_uc) {
            serial_printf("[SYSCALL] ThreadSpawn: no parent user_ctx\n");
            return OB_EPERM;
        }

        int64_t tid = user_thread_spawn(cur, entry, arg, ustack_top);
        if (tid < 0) {
            serial_printf("[SYSCALL] ThreadSpawn failed rc=%lld\n",
                          (long long)tid);
            return tid;
        }
        serial_printf("[SYSCALL] ThreadSpawn parent=%llu tid=%lld "
                      "entry=0x%llx arg=0x%llx ustack=0x%llx\n",
                      (unsigned long long)cur->pid,
                      (long long)tid,
                      (unsigned long long)entry,
                      (unsigned long long)arg,
                      (unsigned long long)ustack_top);
        return tid;
    }

    case SYS_OB_ThreadWait: {
        uint64_t tid = f->rdi;
        int code = 0;
        int rc = task_join(tid, &code);
        if (rc != 0) return rc;
        return (int64_t)code;
    }

    case SYS_OB_GetTid: {
        struct thread *th = sched_current();
        return (int64_t)(th ? th->tid : 0);
    }

    case SYS_OB_Pipe: {
        uint64_t uaddr = f->rdi;
        if (!user_range_ok(uaddr, sizeof(int) * 2)) return OB_EFAULT;

        extern int pipefs_create(struct task_t *cur, uint64_t uaddr);
        return pipefs_create(cur, uaddr);
    }

    case SYS_OB_ShmCreate: {
        uint64_t size = f->rdi;
        extern int64_t shm_sys_create(struct task_t *cur, uint64_t size);
        return shm_sys_create(cur, size);
    }

    case SYS_OB_ShmMap: {
        uint64_t shm_id = f->rdi;
        uint64_t uaddr  = f->rsi;
        extern int64_t shm_sys_map(struct task_t *cur, uint64_t shm_id,
                                    uint64_t uaddr);
        return shm_sys_map(cur, shm_id, uaddr);
    }

    case SYS_OB_ShmUnmap: {
        uint64_t uaddr = f->rdi;
        uint64_t size  = f->rsi;
        extern int shm_sys_unmap(struct task_t *cur, uint64_t uaddr,
                                  uint64_t size);
        return shm_sys_unmap(cur, uaddr, size);
    }

    case SYS_OB_Poll: {
        uint64_t fds_uaddr = f->rdi;
        uint32_t nfds      = (uint32_t)f->rsi;
        int32_t  timeout_ms = (int32_t)f->rdx;
        extern int64_t select_poll(struct task_t *cur, uint64_t fds_uaddr,
                                    uint32_t nfds, int32_t timeout_ms);
        return select_poll(cur, fds_uaddr, nfds, timeout_ms);
    }

    case SYS_OB_Select: {
        uint32_t nfds = (uint32_t)f->rdi;
        uint64_t rfd  = f->rsi;
        uint64_t wfd  = f->rdx;
        extern int64_t select_select(struct task_t *cur, uint32_t nfds,
                                      uint64_t rfd, uint64_t wfd);
        return select_select(cur, nfds, rfd, wfd);
    }

    case SYS_OB_EpollCreate:
    case SYS_OB_EpollCtl:
    case SYS_OB_EpollWait:
        return OB_ENOSYS;

    case SYS_OB_SetCpuQuota: {
        uint64_t target = f->rdi;
        uint32_t quota  = (uint32_t)f->rsi;

        if (cur->privilege_level < 6 && target != cur->pid) {
            return OB_EPERM;
        }
        if (quota > 100) return OB_EINVAL;

        struct task_t *t = task_find_by_pid(target);
        if (!t) return OB_ENOENT;
        t->cpu_usage_quota = quota;

        serial_printf("[SYSCALL] SetCpuQuota pid=%llu quota=%u\n",
                      (unsigned long long)target, (unsigned)quota);
        return 0;
    }

    /*
     * ★★★ 修复 2：SYS_OB_SetMemQuota 真实实现 ★★★
     *
     * 人工必须审查：
     *   - 权限 >= 6 可修改任意 pid 的配额；
     *   - 其他进程只能修改自己的配额（自限）；
     *   - pages == 0 表示"不限制"，但为了避免误改，只有当 pages < 1000
     *     且 > 0 时才生效（保留原有的保护）；
     *   - 超限由 user_map_pages 检查，本函数仅设置配额值。
     */
    case SYS_OB_SetMemQuota: {
        uint64_t target = f->rdi;
        uint32_t pages  = (uint32_t)f->rsi;

        if (cur->privilege_level < 6 && target != cur->pid) {
            return OB_EPERM;
        }

        struct task_t *t = task_find_by_pid(target);
        if (!t) return OB_ENOENT;

        if (pages > 0 && pages < 1000) {
            t->mem_quota_pages = pages;
            serial_printf("[SYSCALL] SetMemQuota pid=%llu pages=%u\n",
                          (unsigned long long)target, (unsigned)pages);
        }
        return 0;
    }

    case SYS_OB_Seccomp: {
        uint64_t mode = f->rdi;
        uint64_t filt = f->rsi;
        return seccomp_syscall(cur, mode, filt);
    }

    case SYS_OB_Ptrace: {
        uint64_t request = f->rdi;
        uint64_t pid     = f->rsi;
        uint64_t addr    = f->rdx;
        uint64_t data    = f->r10;
        extern int64_t ptrace_syscall(struct task_t *cur, uint64_t request,
                                       uint64_t pid, uint64_t addr,
                                       uint64_t data);
        return ptrace_syscall(cur, request, pid, addr, data);
    }

    /* ★★★ 修复 9：clone 语义与 unshare 分离 ★★★ */
    case SYS_OB_Clone: {
        uint64_t cflags = f->rdi;
        uint64_t stack  = f->rsi;
        (void)stack;

        if (cur->privilege_level < 2) return OB_EPERM;

        extern int64_t namespace_clone_child(struct task_t *cur,
                                              uint64_t flags);
        return namespace_clone_child(cur, cflags);
    }

    case SYS_OB_Unshare: {
        uint64_t uflags = f->rdi;
        extern int64_t namespace_unshare(struct task_t *cur, uint64_t flags);
        return namespace_unshare(cur, uflags);
    }

    default:
        serial_printf("[SYSCALL] unknown nr=0x%llx pid=%llu\n",
                      (unsigned long long)nr,
                      (unsigned long long)cur->pid);
        return OB_ENOSYS;
    }
}
/*===OmniBridgeOs/kernel/arch/x64/syscall.c 结束===*/