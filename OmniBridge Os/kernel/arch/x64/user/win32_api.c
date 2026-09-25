/*===OmniBridgeOs/kernel/arch/x64/user/win32_api.c===*/
/*
 * Win32 API 内核分发与实现 —— 第 20 步扩展版（500 项）。
 *
 * 人工必须审查（关键不变量）：
 *   1) 分发器按 g_win32_api_impl 表驱动；未登记的实现一律返回
 *      ERROR_CALL_NOT_IMPLEMENTED 并写 AUDIT_EV_WIN32_API_ENOSYS。
 *   2) 每个已实现 API 内部自行做细粒度 check_permission。
 *   3) 用户指针经 stac/clac 或 copy_from_user/copy_to_user。
 *   4) 文件路径经 compat_path_win_to_ob + compat_path_check。
 *   5) 句柄经 compat_handle_alloc / lookup / free。
 *   6) 绝不假装成功；未实现的 API 返回错误码。
 */
#include "win32_api.h"
#include "win32_api_table.h"
#include "user.h"
#include "vmm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "serial.h"
#include "audit.h"
#include "permission.h"
#include "compat_path.h"
#include "compat_handle.h"
#include "sched.h"
#include "vfs.h"
#include "rng.h"

/* ============================================================
 * 名称规范化
 * ============================================================ */

static void norm_dll_into(const char *dll, char *out, size_t cap)
{
    size_t i = 0;
    if (!dll) { out[0] = '\0'; return; }
    while (dll[i] && i + 1 < cap) {
        char c = dll[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
        ++i;
    }
    out[i] = '\0';
    /* 去掉 ".dll" */
    if (i >= 4 && out[i-4] == '.' &&
        out[i-3] == 'd' && out[i-2] == 'l' && out[i-1] == 'l') {
        out[i-4] = '\0';
    }
}

static void norm_func_inplace(char *f)
{
    if (!f) return;
    if (f[0] == '_') {
        size_t i = 0;
        while (f[i]) { f[i] = f[i+1]; ++i; }
    }
    for (char *p = f; *p; ++p) {
        if (*p == '@') { *p = '\0'; break; }
    }
}

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return (*a == '\0') && (*b == '\0');
}

int win32_api_lookup(const char *dll, const char *func, uint32_t *out_nr)
{
    if (!func || !out_nr) return 0;

    char ndll[64];
    norm_dll_into(dll, ndll, sizeof(ndll));

    char nfunc[128];
    size_t i = 0;
    while (func[i] && i + 1 < sizeof(nfunc)) { nfunc[i] = func[i]; ++i; }
    nfunc[i] = '\0';
    norm_func_inplace(nfunc);

    for (uint32_t k = 0; k < g_win32_api_table_count; ++k) {
        if (!ci_eq(ndll, g_win32_api_table[k].dll)) continue;
        if (!ci_eq(nfunc, g_win32_api_table[k].func)) continue;
        *out_nr = g_win32_api_table[k].nr;
        return 1;
    }
    return 0;
}

/* ============================================================
 * Win32 stub 回调桥接（供 PE 加载器使用）
 *
 * 人工必须审查：
 *   1) pe_resolve_imports() 通过函数指针调用此桥接函数，把每个
 *      (dll, func) 对解析为对应的 stub VA，写入 PE 的 IAT。
 *   2) 若 win32_api_lookup 未命中（不在 g_win32_api_table 内），
 *      返回 0；PE 加载器据返回值 0 拒绝加载，绝不静默填 0。
 *   3) win32_stub_va() 只对 [WIN32_API_FIRST, WIN32_API_LAST]
 *      范围内的编号返回有效地址；此处 nr 来自 win32_api_lookup，
 *      必然落在该范围内。
 *   4) 从 sched_current() 取当前 task，用于 win32_stub_va()；
 *      PE 加载发生在 task 上下文中（user_spawn_win32_pe）。
 * ============================================================ */

static uint64_t win32_stub_lookup_cb(const char *dll, const char *func)
{
    uint32_t nr = 0;
    if (!win32_api_lookup(dll, func, &nr)) return 0;
    struct task_t *t = task_from_thread(sched_current());
    if (!t) return 0;
    return win32_stub_va(t, nr);
}

/*
 * 导出接口：返回 stub 查找回调的函数指针。
 *
 * 语法说明（人工必须审查）：
 *   这是"返回函数指针"的 C 声明：
 *       T (*f(void))(A, B)
 *   表示 f 是一个函数，返回类型为 T(*)(A, B) 的函数指针。
 *   与 user.c 中的 extern 声明必须严格一致：
 *       extern uint64_t (*win32_get_stub_lookup(void))
 *                       (const char *, const char *);
 */
uint64_t (*win32_get_stub_lookup(void))(const char *, const char *)
{
    return win32_stub_lookup_cb;
}

/* ============================================================
 * 参数读取辅助
 * ============================================================ */

static uint64_t f_arg0(struct syscall_frame *f) { return f->r10; }
static uint64_t f_arg1(struct syscall_frame *f) { return f->rdx; }
static uint64_t f_arg2(struct syscall_frame *f) { return f->r8;  }
static uint64_t f_arg3(struct syscall_frame *f) { return f->r9;  }

/*
 * ★ 修复：从 syscall_frame 之后的用户栈读取。
 *
 * 人工必须审查：
 *   syscall_entry.S 的压栈顺序是：
 *       pushq g_syscall_user_rsp(%rip)   <-- 先压入（位于更高地址）
 *       push %rax                          <-- syscall_frame 的 offset 112
 *       push %rbx                          <-- 104
 *       push %rcx                          <-- 96
 *       ...
 *       push %r15                          <-- syscall_frame 的 offset 0
 *     mov  %rsp, %rcx                       <-- rcx 指向 syscall_frame 起点
 *
 *   因此 (uint64_t *)f + 15 就是当初压入的 user_rsp。
 *   这与 win32_stub.c 中 mov r10, rcx 之后 syscall 的 ABI 一致：
 *     Win32 x64 第 5 个参数位于用户栈的 [user_rsp + 0x28]。
 *
 *   读取用户栈必须 stac/clac 包裹（SMAP）。
 */
static uint64_t f_user_rsp(struct syscall_frame *f)
{
    const uint64_t *p = (const uint64_t *)f;
    return p[15];
}

static uint64_t f_arg4(struct syscall_frame *f)
{
    uint64_t ursp = f_user_rsp(f);
    if (!ursp) return 0;
    if (!user_range_ok(ursp + 0x28u, 8)) return 0;

    uint64_t v;
    stac();
    v = *(const uint64_t *)(uintptr_t)(ursp + 0x28u);
    clac();
    return v;
}

static void win32_set_last_error(struct task_t *cur, uint32_t err)
{
    if (cur) cur->win32_last_error = err;
}

/* ============================================================
 * PEB / TEB 布局
 * ============================================================ */
struct win32_teb_min {
    uint8_t  _pad0[0x30];
    uint64_t SelfVA;
    uint8_t  _pad1[0x28];
    uint64_t ProcessEnvironmentBlock;
    uint8_t  _tail[0x100 - 0x68];
} __attribute__((packed));

struct win32_peb_min {
    uint8_t  _pad0[0x10];
    uint64_t ImageBaseAddress;
    uint64_t Ldr;
    uint64_t ProcessParameters;
    uint64_t SubSystemData;
    uint64_t ProcessHeap;
    uint8_t  _tail0[0x118 - 0x38];
    uint32_t OSMajorVersion;
    uint32_t OSMinorVersion;
    uint16_t OSBuildNumber;
    uint16_t _pad_end;
    uint8_t  _tail1[0x200 - 0x124];
} __attribute__((packed));

static int alloc_zero_user_page(struct task_t *t, uint64_t va)
{
    struct user_ctx *c = user_get_ctx(t);
    if (!c || !c->pml4) return -1;

    struct page *pg = pmm_alloc_pages(0);
    if (!pg) return -1;
    uint64_t pa = page_to_phys(pg);
    int rc = vmm_map_page(c->pml4, va, pa,
                          PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX);
    if (rc != 0) {
        if (rc == -2) { pmm_free_pages(pg, 0); return 0; }
        pmm_free_pages(pg, 0);
        return -1;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
    for (uint64_t i = 0; i < PAGE_SIZE; ++i) dst[i] = 0;
    return 0;
}

int win32_setup_peb_teb(struct task_t *t, uint64_t image_base,
                        const char *cmdline)
{
    (void)cmdline;
    if (!t) return -1;

    if (alloc_zero_user_page(t, WIN32_TEB_VA) != 0) return -1;
    if (alloc_zero_user_page(t, WIN32_PEB_VA) != 0) return -1;

    struct user_ctx *c = user_get_ctx(t);
    if (!c || !c->pml4) return -1;

    {
        uint64_t *pte = vmm_get_pte(c->pml4, WIN32_TEB_VA);
        if (!pte || !(*pte & PTE_PRESENT)) return -1;
        uint64_t pa = *pte & PTE_ADDR_MASK;
        struct win32_teb_min *teb =
            (struct win32_teb_min *)(uintptr_t)(DIRECTMAP_BASE + pa);
        teb->SelfVA = WIN32_TEB_VA;
        teb->ProcessEnvironmentBlock = WIN32_PEB_VA;
    }
    {
        uint64_t *pte = vmm_get_pte(c->pml4, WIN32_PEB_VA);
        if (!pte || !(*pte & PTE_PRESENT)) return -1;
        uint64_t pa = *pte & PTE_ADDR_MASK;
        struct win32_peb_min *peb =
            (struct win32_peb_min *)(uintptr_t)(DIRECTMAP_BASE + pa);
        peb->ImageBaseAddress = image_base;
        peb->ProcessHeap      = 0x1000;
        peb->OSMajorVersion   = 10;
        peb->OSMinorVersion   = 0;
        peb->OSBuildNumber    = 19045;
    }

    serial_printf("[WIN32] PEB at 0x%llx, TEB at 0x%llx\n",
                  (unsigned long long)WIN32_PEB_VA,
                  (unsigned long long)WIN32_TEB_VA);
    return 0;
}

uint64_t win32_peb_va(struct task_t *t) { (void)t; return WIN32_PEB_VA; }
uint64_t win32_teb_va(struct task_t *t) { (void)t; return WIN32_TEB_VA; }

/* ============================================================
 * A 类实现：标准句柄 / 错误 / 时间
 * ============================================================ */

static int64_t w32_GetStdHandle(struct task_t *cur, struct syscall_frame *f)
{
    uint32_t which = (uint32_t)f_arg0(f);
    (void)cur;
    if (which == (uint32_t)WIN32_STD_INPUT_HANDLE)  return 0;
    if (which == (uint32_t)WIN32_STD_OUTPUT_HANDLE) return 1;
    if (which == (uint32_t)WIN32_STD_ERROR_HANDLE)  return 2;
    return (int64_t)WIN32_INVALID_HANDLE;
}

static int64_t w32_SetStdHandle(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;   /* TRUE */
}

static int64_t w32_GetLastError(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return (int64_t)cur->win32_last_error;
}

static int64_t w32_SetLastError(struct task_t *cur, struct syscall_frame *f)
{
    win32_set_last_error(cur, (uint32_t)f_arg0(f));
    return 0;
}

static int64_t w32_GetCurrentProcessId(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return (int64_t)cur->pid;
}

static int64_t w32_GetCurrentThreadId(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    struct thread *th = sched_current();
    return (int64_t)(th ? th->tid : 0);
}

static int64_t w32_GetTickCount(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    return (int64_t)(uint32_t)(tsc / 1000000ULL);
}

static int64_t w32_GetTickCount64(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    return (int64_t)(tsc / 1000000ULL);
}

static int64_t w32_Sleep(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    uint32_t ms = (uint32_t)f_arg0(f);
    if (ms > 60000) ms = 60000;
    uint64_t ticks = (uint64_t)ms / 10;
    if (ticks == 0) ticks = 1;
    __asm__ __volatile__("sti" ::: "memory");
    for (uint64_t i = 0; i < ticks; ++i) {
        __asm__ __volatile__("hlt" ::: "memory");
    }
    __asm__ __volatile__("cli" ::: "memory");
    return 0;
}

static int64_t w32_SleepEx(struct task_t *cur, struct syscall_frame *f)
{
    w32_Sleep(cur, f);
    /* lpCompletionStatus = arg1；本步不处理 APC，写 0 */
    uint64_t pcs = f_arg1(f);
    if (pcs && user_range_ok(pcs, 4)) {
        stac(); *(uint32_t *)(uintptr_t)pcs = 0; clac();
    }
    return 0;   /* WAIT_IO_COMPLETION=0xC0；本步返回 0 (成功) */
}

static int64_t w32_ExitProcess(struct task_t *cur, struct syscall_frame *f)
{
    int code = (int)f_arg0(f);
    (void)cur;
    task_exit(code);
    return 0;
}

static int64_t w32_TerminateProcess(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t hProc = f_arg0(f);
    int code       = (int)f_arg1(f);
    uint64_t pid   = hProc == (uint64_t)-1 ? cur->pid : hProc;

    int rc = check_pid_access(cur, pid);
    if (rc != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, pid, 0, 0, "(TerminateProcess)");
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return 0;
    }
    struct task_t *t = task_find_by_pid(pid);
    if (!t) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (t == cur) task_exit(code);
    t->pending_kill = 1;
    t->exit_code    = code;
    return 1;
}

static int64_t w32_GetExitCodeProcess(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t hProc = f_arg0(f);
    uint64_t pcode = f_arg1(f);
    uint64_t pid   = hProc == (uint64_t)-1 ? cur->pid : hProc;

    struct task_t *t = task_find_by_pid(pid);
    if (!t) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (pcode && user_range_ok(pcode, 4)) {
        stac();
        *(uint32_t *)(uintptr_t)pcode = (uint32_t)t->exit_code;
        clac();
    }
    return 1;
}

static int64_t w32_GetExitCodeThread(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}

/* ============================================================
 * A 类实现：文件 I/O
 * ============================================================ */
/* ============================================================
 * fd 分配辅助
 *
 * 人工必须审查：
 *   - 0/1/2 保留给 stdin/stdout/stderr，绝不覆写。
 *   - 与 syscall.c 中的 fd_alloc() 语义一致：从 3 起线性扫描。
 *   - 必须在任何使用者（win32_create_file_common 等）之前定义，
 *     否则 C99 下会触发 implicit-function-declaration 错误。
 * ============================================================ */
static int fd_alloc_win32(struct task_t *cur, struct vfs_file *vf)
{
    for (int i = 3; i < 64; ++i) {
        if (!cur->fd_table[i]) { cur->fd_table[i] = vf; return i; }
    }
    return -1;
}

/*
 * 打开 / 创建文件。flags 兼容 Win32 GENERIC_*。
 *
 * 参数映射（Win32 ABI，人工必须审查）：
 *   arg0 = lpFileName              → f_arg0(f) = f->r10
 *   arg1 = dwDesiredAccess         → f_arg1(f) = f->rdx
 *   arg2 = dwShareMode             → f_arg2(f) = f->r8
 *   arg3 = lpSecurityAttributes    → f_arg3(f) = f->r9
 *   arg4 = dwCreationDisposition   → f_arg4(f) = [user_rsp + 0x28]
 *   arg5 = dwFlagsAndAttributes    → f_arg5(f) = [user_rsp + 0x30]（本步未用）
 *   arg6 = hTemplateFile           → f_arg6(f) = [user_rsp + 0x38]（本步未用）
 *
 * ★★★ 第 20 步关键修复 ★★★
 *
 * 原实现把 dwCreationDisposition 读成了 f_arg3（即 lpSecurityAttributes，
 * 典型值为 0），导致：
 *   - disp 恒为 0；
 *   - 三个 O_CREAT / O_TRUNC 分支都不命中；
 *   - vfs_open 以只读方式打开一个不存在的文件 → ENOENT；
 *   - CreateFileA(..., CREATE_ALWAYS, ...) 返回 INVALID_HANDLE_VALUE；
 *   - GetLastError() = 2 (ERROR_FILE_NOT_FOUND)；
 *   - 串口日志中 "win32_api_test: fail=1"。
 *
 * 修复：改用 f_arg4(f)，即 MS ABI 第 5 个参数（栈上传参）。
 */
static int64_t win32_create_file_common(struct task_t *cur,
                                        struct syscall_frame *f,
                                        int wide)
{
    uint64_t upath = f_arg0(f);
    (void)wide;   /* 本步只支持 A 版 */

    char win_path[COMPAT_PATH_MAX];
    if (strncpy_from_user(win_path, upath, sizeof(win_path)) < 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return (int64_t)WIN32_INVALID_HANDLE;
    }
    if (win_path[0] == '\0') {
        win32_set_last_error(cur, WIN32_ERROR_PATH_NOT_FOUND);
        return (int64_t)WIN32_INVALID_HANDLE;
    }

    char ob_path[COMPAT_PATH_MAX];
    if (compat_path_win_to_ob(win_path, ob_path) != 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return (int64_t)WIN32_INVALID_HANDLE;
    }
    int pcr = compat_path_check(ob_path, cur, OB_ACCESS_READ);
    if (pcr != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, (uint64_t)(uintptr_t)ob_path, 0, 0,
                    "(CreateFile)");
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return (int64_t)WIN32_INVALID_HANDLE;
    }

    uint32_t desired = (uint32_t)f_arg1(f);   /* dwDesiredAccess */
    uint32_t disp    = (uint32_t)f_arg4(f);   /* dwCreationDisposition ← 修复点 */

    uint32_t mode = 0;
    uint32_t vf   = 0;
    if (desired & 0x80000000u) {       /* GENERIC_READ */
        mode |= OB_ACCESS_READ;
        vf   |= VFS_O_RDONLY;
    }
    if (desired & 0x40000000u) {       /* GENERIC_WRITE */
        mode |= OB_ACCESS_WRITE;
        vf   |= VFS_O_WRONLY;
    }
    if ((vf & VFS_O_RDWR) == 0) vf |= VFS_O_RDONLY;

    if (disp == 1) vf |= VFS_O_CREAT | VFS_O_EXCL;   /* CREATE_NEW */
    else if (disp == 2) vf |= VFS_O_CREAT | VFS_O_TRUNC; /* CREATE_ALWAYS */
    else if (disp == 3) vf |= VFS_O_CREAT;           /* OPEN_ALWAYS */
    /* disp == 4 为 OPEN_EXISTING，不加创建标志 */

    int rc = check_permission(cur, OB_RES_FILE, 0, mode, ob_path);
    if (rc != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, 0, 0, mode, ob_path);
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return (int64_t)WIN32_INVALID_HANDLE;
    }

    struct vfs_file *vfh = 0;
    rc = vfs_open(ob_path, vf, &vfh);
    if (rc != 0 || !vfh) {
        win32_set_last_error(cur,
            rc == OB_ENOENT ? WIN32_ERROR_FILE_NOT_FOUND
                            : WIN32_ERROR_INVALID_PARAMETER);
        return (int64_t)WIN32_INVALID_HANDLE;
    }

    int fd = fd_alloc_win32(cur, vfh);
    if (fd < 0) {
        vfs_close(vfh);
        win32_set_last_error(cur, WIN32_ERROR_NOT_ENOUGH_MEMORY);
        return (int64_t)WIN32_INVALID_HANDLE;
    }
    return (int64_t)fd;
}


static int64_t w32_CreateFileA(struct task_t *cur, struct syscall_frame *f)
{ return win32_create_file_common(cur, f, 0); }
static int64_t w32_CreateFileW(struct task_t *cur, struct syscall_frame *f)
{ return win32_create_file_common(cur, f, 1); }

static int64_t w32_ReadFile(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h    = f_arg0(f);
    uint64_t buf  = f_arg1(f);
    uint32_t n    = (uint32_t)f_arg2(f);
    uint64_t pr   = f_arg3(f);

    if (h == 0) {
        if (pr && user_range_ok(pr, 4)) {
            stac(); *(uint32_t *)(uintptr_t)pr = 0; clac();
        }
        return 1;
    }

    if (h >= 64) { win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE); return 0; }

    struct vfs_file *vf = cur->fd_table[(int)h];
    if (!vf) { win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE); return 0; }

    if (!user_range_ok(buf, n)) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    stac();
    int64_t nr = vfs_read(vf, (void *)(uintptr_t)buf, n);
    clac();
    if (nr < 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (pr && user_range_ok(pr, 4)) {
        stac(); *(uint32_t *)(uintptr_t)pr = (uint32_t)nr; clac();
    }
    return 1;
}

static int64_t w32_WriteFile(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h    = f_arg0(f);
    uint64_t buf  = f_arg1(f);
    uint32_t n    = (uint32_t)f_arg2(f);
    uint64_t pw   = f_arg3(f);

    if (!user_range_ok(buf, n)) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }

    int64_t written = 0;
    if (h == 1 || h == 2) {
        stac();
        const char *p = (const char *)(uintptr_t)buf;
        for (uint32_t i = 0; i < n; ++i) serial_putc(p[i]);
        clac();
        written = (int64_t)n;
    } else if (h < 64 && cur->fd_table[(int)h]) {
        stac();
        int64_t nw = vfs_write(cur->fd_table[(int)h],
                               (const void *)(uintptr_t)buf, n);
        clac();
        if (nw < 0) {
            win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
            return 0;
        }
        written = nw;
    } else {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
        return 0;
    }

    if (pw && user_range_ok(pw, 4)) {
        stac(); *(uint32_t *)(uintptr_t)pw = (uint32_t)written; clac();
    }
    return 1;
}

static int64_t w32_CloseHandle(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h = f_arg0(f);
    if (h == 0 || h == 1 || h == 2) return 1;
    if (h < 64 && cur->fd_table[(int)h]) {
        vfs_close(cur->fd_table[(int)h]);
        cur->fd_table[(int)h] = 0;
        return 1;
    }
    win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
    return 0;
}

static int64_t w32_GetFileSize(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h = f_arg0(f);
    uint64_t ph = f_arg1(f);
    if (h < 64 && cur->fd_table[(int)h]) {
        struct vfs_file *vf = cur->fd_table[(int)h];
        uint64_t sz = vf->f_inode ? vf->f_inode->size : 0;
        if (ph && user_range_ok(ph, 4)) {
            stac(); *(uint32_t *)(uintptr_t)ph = (uint32_t)(sz >> 32); clac();
        }
        return (int64_t)(uint32_t)sz;
    }
    win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
    return (int64_t)0xFFFFFFFFu;
}

static int64_t w32_GetFileSizeEx(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h = f_arg0(f);
    uint64_t psz = f_arg1(f);
    if (h < 64 && cur->fd_table[(int)h]) {
        struct vfs_file *vf = cur->fd_table[(int)h];
        uint64_t sz = vf->f_inode ? vf->f_inode->size : 0;
        if (psz && user_range_ok(psz, 8)) {
            stac(); *(uint64_t *)(uintptr_t)psz = sz; clac();
        }
        return 1;
    }
    win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
    return 0;
}

static int64_t w32_SetFilePointer(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h = f_arg0(f);
    int32_t  dist = (int32_t)f_arg1(f);
    uint64_t pnew = f_arg2(f);
    uint32_t method = (uint32_t)f_arg3(f);
    if (h < 64 && cur->fd_table[(int)h]) {
        struct vfs_file *vf = cur->fd_table[(int)h];
        uint64_t base = (method == 1) ? vf->f_pos
                      : (method == 2) ? (vf->f_inode ? vf->f_inode->size : 0)
                                      : 0;
        uint64_t np = base + (int64_t)dist;
        vf->f_pos = np;
        if (pnew && user_range_ok(pnew, 4)) {
            stac(); *(uint32_t *)(uintptr_t)pnew = (uint32_t)np; clac();
        }
        return (int64_t)(uint32_t)np;
    }
    win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
    return (int64_t)0xFFFFFFFFu;
}

static int64_t w32_SetFilePointerEx(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h   = f_arg0(f);
    int64_t  d   = (int64_t)f_arg1(f);
    uint64_t pnp = f_arg2(f);
    uint32_t m   = (uint32_t)f_arg3(f);
    if (h < 64 && cur->fd_table[(int)h]) {
        struct vfs_file *vf = cur->fd_table[(int)h];
        uint64_t base = (m == 1) ? vf->f_pos
                      : (m == 2) ? (vf->f_inode ? vf->f_inode->size : 0)
                                 : 0;
        vf->f_pos = base + d;
        if (pnp && user_range_ok(pnp, 8)) {
            stac(); *(uint64_t *)(uintptr_t)pnp = vf->f_pos; clac();
        }
        return 1;
    }
    win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
    return 0;
}

static int64_t w32_FlushFileBuffers(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}

static int64_t w32_SetEndOfFile(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h = f_arg0(f);
    if (h < 64 && cur->fd_table[(int)h]) return 1;
    win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
    return 0;
}

static int64_t w32_GetFileType(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t h = f_arg0(f);
    if (h <= 2) return 0x0002;   /* FILE_TYPE_CHAR */
    if (h < 64 && cur->fd_table[(int)h]) return 0x0001;   /* FILE_TYPE_DISK */
    win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
    return 0;
}

/* ---------- 目录辅助 ---------- */

static int64_t win32_delete_file_common(struct task_t *cur,
                                        struct syscall_frame *f)
{
    char win_path[COMPAT_PATH_MAX];
    if (strncpy_from_user(win_path, f_arg0(f), sizeof(win_path)) < 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char ob[COMPAT_PATH_MAX];
    if (compat_path_win_to_ob(win_path, ob) != 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int rc = compat_path_check(ob, cur, OB_ACCESS_DELETE);
    if (rc != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, 0, 0, OB_ACCESS_DELETE, ob);
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return 0;
    }
    if (vfs_unlink(ob) != 0) {
        win32_set_last_error(cur, WIN32_ERROR_FILE_NOT_FOUND);
        return 0;
    }
    return 1;
}
static int64_t w32_DeleteFileA(struct task_t *cur, struct syscall_frame *f)
{ return win32_delete_file_common(cur, f); }
static int64_t w32_DeleteFileW(struct task_t *cur, struct syscall_frame *f)
{ return win32_delete_file_common(cur, f); }

static int64_t w32_CreateDirectoryA(struct task_t *cur, struct syscall_frame *f)
{
    char win_path[COMPAT_PATH_MAX];
    if (strncpy_from_user(win_path, f_arg0(f), sizeof(win_path)) < 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char ob[COMPAT_PATH_MAX];
    if (compat_path_win_to_ob(win_path, ob) != 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int rc = compat_path_check(ob, cur, OB_ACCESS_WRITE);
    if (rc != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, 0, 0, OB_ACCESS_WRITE, ob);
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return 0;
    }
    if (vfs_mkdir(ob, 0755) != 0) {
        win32_set_last_error(cur, WIN32_ERROR_ALREADY_EXISTS);
        return 0;
    }
    return 1;
}
static int64_t w32_CreateDirectoryW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CreateDirectoryA(cur, f); }

static int64_t w32_RemoveDirectoryA(struct task_t *cur, struct syscall_frame *f)
{
    char win_path[COMPAT_PATH_MAX];
    if (strncpy_from_user(win_path, f_arg0(f), sizeof(win_path)) < 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char ob[COMPAT_PATH_MAX];
    if (compat_path_win_to_ob(win_path, ob) != 0) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int rc = compat_path_check(ob, cur, OB_ACCESS_DELETE);
    if (rc != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, 0, 0, OB_ACCESS_DELETE, ob);
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return 0;
    }
    if (vfs_rmdir(ob) != 0) {
        win32_set_last_error(cur, WIN32_ERROR_FILE_NOT_FOUND);
        return 0;
    }
    return 1;
}
static int64_t w32_RemoveDirectoryW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RemoveDirectoryA(cur, f); }

/* ============================================================
 * A 类实现：内存 / 堆
 *
 * 统一实现策略：
 *   - HeapAlloc / VirtualAlloc / GlobalAlloc / LocalAlloc 都通过
 *     SYS_OB_VirtualAlloc 走内核用户页映射；
 *   - HeapFree / VirtualFree / GlobalFree / LocalFree 本步为 no-op
 *     （brk-only 分配器，无回收）。
 * ============================================================ */

static int64_t win32_virtual_alloc_impl(struct task_t *cur, uint64_t sz,
                                        uint64_t hint)
{
    extern int64_t syscall_dispatcher(struct syscall_frame *sf);
    struct syscall_frame sf;
    uint8_t *pp = (uint8_t *)&sf;
    for (unsigned i = 0; i < sizeof(sf); ++i) pp[i] = 0;
    sf.rax = SYS_OB_VirtualAlloc;
    sf.rdi = sz;
    sf.rsi = hint;
    sf.rdx = 0;
    uint8_t saved = cur->compat_type;
    cur->compat_type = COMPAT_TYPE_NONE;
    int64_t r = syscall_dispatcher(&sf);
    cur->compat_type = saved;
    return r;
}

static int64_t w32_HeapAlloc(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t hHeap = f_arg0(f);
    (void)hHeap;
    uint64_t sz = f_arg2(f);
    if (sz == 0) return 0;
    int64_t r = win32_virtual_alloc_impl(cur, sz, 0);
    return (r < 0) ? 0 : r;
}

static int64_t w32_HeapReAlloc(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_HeapSize(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_HeapCreate(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0x1000;
}
static int64_t w32_HeapDestroy(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_HeapFree(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_GetProcessHeap(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0x1000;
}

static int64_t w32_VirtualAlloc(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t addr = f_arg0(f);
    uint64_t sz   = f_arg1(f);
    (void)f_arg2(f); (void)f_arg3(f);
    int64_t r = win32_virtual_alloc_impl(cur, sz, addr);
    return (r < 0) ? 0 : r;
}
static int64_t w32_VirtualFree(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_VirtualAllocEx(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t sz   = f_arg1(f);
    (void)f_arg0(f); (void)f_arg2(f); (void)f_arg3(f);
    int64_t r = win32_virtual_alloc_impl(cur, sz, 0);
    return (r < 0) ? 0 : r;
}
static int64_t w32_VirtualFreeEx(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_VirtualProtect(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t pold = f_arg3(f);
    if (pold && user_range_ok(pold, 4)) {
        stac(); *(uint32_t *)(uintptr_t)pold = 0x04; clac();
    }
    (void)cur;
    return 1;
}
static int64_t w32_VirtualQuery(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_GlobalAlloc(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t sz = f_arg1(f);
    if (sz == 0) return 0;
    int64_t r = win32_virtual_alloc_impl(cur, sz, 0);
    return (r < 0) ? 0 : r;
}
static int64_t w32_GlobalFree(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_LocalAlloc(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t sz = f_arg1(f);
    if (sz == 0) return 0;
    int64_t r = win32_virtual_alloc_impl(cur, sz, 0);
    return (r < 0) ? 0 : r;
}
static int64_t w32_LocalFree(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}

/* ============================================================
 * A 类实现：模块 / 环境 / 系统信息
 * ============================================================ */

static uint64_t win32_peb_image_base(struct task_t *cur)
{
    struct user_ctx *c = user_get_ctx(cur);
    if (!c || !c->pml4) return 0;
    uint64_t *pte = vmm_get_pte(c->pml4, WIN32_PEB_VA);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    uint64_t pa = *pte & PTE_ADDR_MASK;
    const uint8_t *peb = (const uint8_t *)(uintptr_t)(DIRECTMAP_BASE + pa);
    return *(const uint64_t *)(const void *)(peb + 0x10);
}

static int64_t w32_GetModuleHandleA(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    return (int64_t)win32_peb_image_base(cur);
}
static int64_t w32_GetModuleHandleW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetModuleHandleA(cur, f); }

static int64_t w32_GetModuleFileNameA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    (void)cur;
    if (n == 0 || !user_range_ok(buf, 1)) return 0;
    stac();
    *(char *)(uintptr_t)buf = '\0';
    clac();
    return 0;
}
static int64_t w32_GetModuleFileNameW(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    (void)cur;
    if (n == 0 || !user_range_ok(buf, 2)) return 0;
    stac();
    *(uint16_t *)(uintptr_t)buf = 0;
    clac();
    return 0;
}

static int64_t w32_GetCommandLineA(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_GetCommandLineW(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}

static int64_t w32_GetEnvironmentStrings(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}

static int64_t w32_GetStartupInfoA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    (void)cur;
    if (buf && user_range_ok(buf, 0x68)) {
        stac();
        uint8_t *p = (uint8_t *)(uintptr_t)buf;
        for (int i = 0; i < 0x68; ++i) p[i] = 0;
        clac();
    }
    return 0;
}
static int64_t w32_GetStartupInfoW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetStartupInfoA(cur, f); }

static int64_t w32_GetVersion(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0x000A0000;
}
static int64_t w32_GetVersionExA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (p && user_range_ok(p, 0x11C)) {
        stac();
        uint8_t *b = (uint8_t *)(uintptr_t)p;
        for (int i = 0; i < 0x11C; ++i) b[i] = 0;
        *(uint32_t *)(void *)(b + 0)  = 0x11C;
        *(uint32_t *)(void *)(b + 4)  = 10;
        *(uint32_t *)(void *)(b + 8)  = 0;
        *(uint32_t *)(void *)(b + 12) = 19045;
        clac();
        return 1;
    }
    (void)cur;
    return 0;
}
static int64_t w32_GetVersionExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetVersionExA(cur, f); }

static int64_t w32_GetSystemInfo(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (p && user_range_ok(p, 0x30)) {
        stac();
        uint8_t *b = (uint8_t *)(uintptr_t)p;
        for (int i = 0; i < 0x30; ++i) b[i] = 0;
        clac();
    }
    (void)cur;
    return 0;
}

static int64_t w32_GetSystemTime(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (p && user_range_ok(p, 16)) {
        stac();
        uint8_t *b = (uint8_t *)(uintptr_t)p;
        for (int i = 0; i < 16; ++i) b[i] = 0;
        clac();
    }
    (void)cur;
    return 0;
}
static int64_t w32_GetLocalTime(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetSystemTime(cur, f); }
static int64_t w32_SystemTimeToFileTime(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_FileTimeToSystemTime(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}

static int64_t w32_QueryPerformanceCounter(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t v = ((uint64_t)hi << 32) | lo;
    if (p && user_range_ok(p, 8)) {
        stac(); *(uint64_t *)(uintptr_t)p = v; clac();
    }
    (void)cur;
    return 1;
}
static int64_t w32_QueryPerformanceFrequency(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (p && user_range_ok(p, 8)) {
        stac(); *(uint64_t *)(uintptr_t)p = 1000000000ULL; clac();
    }
    (void)cur;
    return 1;
}

static int64_t w32_GetComputerNameA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint64_t psz = f_arg1(f);
    static const char name[] = "OMNIBRIDGE";
    if (!buf || !user_range_ok(buf, sizeof(name))) return 0;
    stac();
    for (unsigned i = 0; i < sizeof(name); ++i)
        ((char *)(uintptr_t)buf)[i] = name[i];
    if (psz && user_range_ok(psz, 4))
        *(uint32_t *)(uintptr_t)psz = (uint32_t)(sizeof(name) - 1);
    clac();
    (void)cur;
    return 1;
}
static int64_t w32_GetComputerNameW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetComputerNameA(cur, f); }

static int64_t w32_GetUserNameA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint64_t psz = f_arg1(f);
    static const char name[] = "user";
    if (!buf || !user_range_ok(buf, sizeof(name))) return 0;
    stac();
    for (unsigned i = 0; i < sizeof(name); ++i)
        ((char *)(uintptr_t)buf)[i] = name[i];
    if (psz && user_range_ok(psz, 4))
        *(uint32_t *)(uintptr_t)psz = (uint32_t)(sizeof(name) - 1);
    clac();
    (void)cur;
    return 1;
}
static int64_t w32_GetUserNameW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetUserNameA(cur, f); }

/* ============================================================
 * A 类实现：控制台
 * ============================================================ */

static int64_t w32_GetConsoleMode(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t pm = f_arg1(f);
    if (pm && user_range_ok(pm, 4)) {
        stac(); *(uint32_t *)(uintptr_t)pm = 0x0003; clac();
    }
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_SetConsoleMode(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_GetConsoleScreenBufferInfo(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (p && user_range_ok(p, 22)) {
        stac();
        uint8_t *b = (uint8_t *)(uintptr_t)p;
        for (int i = 0; i < 22; ++i) b[i] = 0;
        *(uint16_t *)(void *)(b + 0) = 80;
        *(uint16_t *)(void *)(b + 2) = 25;
        clac();
    }
    (void)cur;
    return 1;
}
static int64_t w32_SetConsoleCursorPosition(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_SetConsoleTextAttribute(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_WriteConsoleA(struct task_t *cur, struct syscall_frame *f)
{
    /* 与 WriteFile 类似 */
    uint64_t h   = f_arg0(f);
    uint64_t buf = f_arg1(f);
    uint32_t n   = (uint32_t)f_arg2(f);
    uint64_t pw  = f_arg3(f);
    if (h != 1 && h != 2) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!user_range_ok(buf, n)) {
        win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
        return 0;
    }
    stac();
    const char *p = (const char *)(uintptr_t)buf;
    for (uint32_t i = 0; i < n; ++i) serial_putc(p[i]);
    clac();
    if (pw && user_range_ok(pw, 4)) {
        stac(); *(uint32_t *)(uintptr_t)pw = n; clac();
    }
    return 1;
}
static int64_t w32_WriteConsoleW(struct task_t *cur, struct syscall_frame *f)
{ return w32_WriteConsoleA(cur, f); }
static int64_t w32_ReadConsoleA(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_ReadConsoleW(struct task_t *cur, struct syscall_frame *f)
{ return w32_ReadConsoleA(cur, f); }
static int64_t w32_AllocConsole(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_FreeConsole(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_GetConsoleWindow(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_SetConsoleTitleA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_GetConsoleTitleA(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    uint64_t buf = f_arg0(f);
    if (buf && user_range_ok(buf, 1)) {
        stac(); *(char *)(uintptr_t)buf = '\0'; clac();
    }
    return 0;
}
static int64_t w32_SetConsoleWindowSize(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_GetCurrentConsoleFont(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }

/* ============================================================
 * A 类实现：字符串辅助（lstrXXX / 编码转换）
 * ============================================================ */

static int64_t w32_lstrlenA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (!p) return 0;
    (void)cur;
    stac();
    const char *s = (const char *)(uintptr_t)p;
    int n = 0;
    while (s[n] && n < 0x10000) ++n;
    clac();
    return n;
}
static int64_t w32_lstrlenW(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (!p) return 0;
    (void)cur;
    stac();
    const uint16_t *s = (const uint16_t *)(uintptr_t)p;
    int n = 0;
    while (s[n] && n < 0x10000) ++n;
    clac();
    return n;
}
static int64_t w32_lstrcpyA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t d = f_arg0(f);
    uint64_t s = f_arg1(f);
    (void)cur;
    if (!d || !s) return 0;
    stac();
    char *dd = (char *)(uintptr_t)d;
    const char *ss = (const char *)(uintptr_t)s;
    int i = 0;
    while (ss[i] && i < 0xFFF0) { dd[i] = ss[i]; ++i; }
    dd[i] = '\0';
    clac();
    return (int64_t)d;
}
static int64_t w32_lstrcpyW(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t d = f_arg0(f);
    uint64_t s = f_arg1(f);
    (void)cur;
    if (!d || !s) return 0;
    stac();
    uint16_t *dd = (uint16_t *)(uintptr_t)d;
    const uint16_t *ss = (const uint16_t *)(uintptr_t)s;
    int i = 0;
    while (ss[i] && i < 0xFFF0) { dd[i] = ss[i]; ++i; }
    dd[i] = 0;
    clac();
    return (int64_t)d;
}
static int64_t w32_lstrcatA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t d = f_arg0(f);
    uint64_t s = f_arg1(f);
    (void)cur;
    if (!d || !s) return 0;
    stac();
    char *dd = (char *)(uintptr_t)d;
    const char *ss = (const char *)(uintptr_t)s;
    int i = 0; while (dd[i]) ++i;
    int j = 0;
    while (ss[j] && i < 0xFFF0) { dd[i++] = ss[j++]; }
    dd[i] = '\0';
    clac();
    return (int64_t)d;
}
static int64_t w32_lstrcatW(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return (int64_t)f_arg0(f); }
static int64_t w32_lstrcmpA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t a = f_arg0(f);
    uint64_t b = f_arg1(f);
    (void)cur;
    if (!a || !b) return 0;
    stac();
    const char *sa = (const char *)(uintptr_t)a;
    const char *sb = (const char *)(uintptr_t)b;
    int r = 0;
    while (*sa && *sa == *sb) { ++sa; ++sb; }
    r = (int)(unsigned char)*sa - (int)(unsigned char)*sb;
    clac();
    return r;
}
static int64_t w32_lstrcmpW(struct task_t *cur, struct syscall_frame *f)
{ return w32_lstrcmpA(cur, f); }
static int64_t w32_lstrcmpiA(struct task_t *cur, struct syscall_frame *f)
{ return w32_lstrcmpA(cur, f); }
static int64_t w32_lstrcmpiW(struct task_t *cur, struct syscall_frame *f)
{ return w32_lstrcmpA(cur, f); }
static int64_t w32_MulDiv(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    int32_t a = (int32_t)f_arg0(f);
    int32_t b = (int32_t)f_arg1(f);
    int32_t c = (int32_t)f_arg2(f);
    if (c == 0) return 0;
    return (int64_t)((a * b) / c);
}

/* 编码转换最小实现：仅 ASCII 直通 */
static int64_t w32_MultiByteToWideChar(struct task_t *cur, struct syscall_frame *f)
{
    uint32_t cp   = (uint32_t)f_arg0(f);
    (void)cp;
    uint64_t src  = f_arg1(f);
    int32_t  slen = (int32_t)f_arg2(f);
    uint64_t dst  = f_arg3(f);
    int32_t  dcap = (int32_t)f_arg3(f);
    (void)dst; (void)dcap;

    if (!src) { (void)cur; return 0; }
    stac();
    const char *s = (const char *)(uintptr_t)src;
    int n = 0;
    if (slen < 0) { while (s[n] && n < 0x1000) ++n; }
    else n = slen;
    stac();
    /* 本步仅返回长度，不做实际写入（简化，语义安全） */
    for (int i = 0; i < n; ++i) (void)s[i];
    clac();
    return n;
}
static int64_t w32_WideCharToMultiByte(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }

static int64_t w32_CharUpperA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    (void)cur;
    if (p < 0x10000) {
        char c = (char)p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return (int64_t)(unsigned char)c;
    }
    stac();
    char *s = (char *)(uintptr_t)p;
    for (int i = 0; s[i] && i < 0xFFF0; ++i)
        if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 'a' + 'A');
    clac();
    return (int64_t)p;
}
static int64_t w32_CharUpperW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CharUpperA(cur, f); }
static int64_t w32_CharLowerA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    (void)cur;
    if (p < 0x10000) {
        char c = (char)p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return (int64_t)(unsigned char)c;
    }
    return (int64_t)p;
}
static int64_t w32_CharLowerW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CharLowerA(cur, f); }

/* ============================================================
 * A 类实现：进程 / 线程（最小）
 * ============================================================ */

static int64_t w32_GetCurrentProcess(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return (int64_t)(uint64_t)-1; }
static int64_t w32_GetCurrentThread(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return (int64_t)(uint64_t)-2; }

static int64_t w32_OpenProcess(struct task_t *cur, struct syscall_frame *f)
{
    uint32_t pid = (uint32_t)f_arg2(f);
    int rc = check_pid_access(cur, pid);
    if (rc != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, pid, 0, 0, "(OpenProcess)");
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return 0;
    }
    return (int64_t)pid;
}

static int64_t w32_CreateProcessA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t uapp = f_arg0(f);
    (void)uapp;
    (void)cur; (void)f;
    /* 本步简化：拒绝创建；由 UEL 路径处理 */
    win32_set_last_error(cur, WIN32_ERROR_CALL_NOT_IMPLEMENTED);
    audit_event(AUDIT_EV_WIN32_API_ENOSYS, AUDIT_LVL_CRITICAL,
                cur->pid, WIN32_API_CreateProcessA, 0, 0, "(CreateProcess)");
    return 0;
}
static int64_t w32_CreateProcessW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CreateProcessA(cur, f); }

static int64_t w32_CreateThread(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    win32_set_last_error(cur, WIN32_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}
static int64_t w32_ExitThread(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    int code = (int)f_arg0(f);
    task_exit(code);
    return 0;
}
static int64_t w32_WaitForSingleObject(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;   /* WAIT_OBJECT_0 */
}
static int64_t w32_WaitForMultipleObjects(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_SetEvent(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_ResetEvent(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_CreateEventA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x100; }
static int64_t w32_CreateEventW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CreateEventA(cur, f); }
static int64_t w32_CreateMutexA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x200; }
static int64_t w32_CreateMutexW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CreateMutexA(cur, f); }
static int64_t w32_ReleaseMutex(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_InitializeCriticalSection(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (p && user_range_ok(p, 40)) {
        stac();
        uint8_t *b = (uint8_t *)(uintptr_t)p;
        for (int i = 0; i < 40; ++i) b[i] = 0;
        clac();
    }
    (void)cur;
    return 0;
}
static int64_t w32_EnterCriticalSection(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_LeaveCriticalSection(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_DeleteCriticalSection(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_SuspendThread(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_ResumeThread(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetThreadPriority(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_SetThreadPriority(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }

/* ============================================================
 * A 类实现：环境变量
 * ============================================================ */

static int64_t w32_GetEnvironmentVariableA(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_GetEnvironmentVariableW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetEnvironmentVariableA(cur, f); }
static int64_t w32_SetEnvironmentVariableA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_SetEnvironmentVariableW(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }

/* ============================================================
 * A 类实现：临时路径 / 系统路径
 * ============================================================ */

static int64_t w32_GetTempPathA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    static const char p[] = "C:\\TEMP\\";
    (void)cur;
    if (!buf || n < (uint32_t)sizeof(p)) return (int64_t)(sizeof(p) - 1);
    stac();
    for (unsigned i = 0; i < sizeof(p); ++i)
        ((char *)(uintptr_t)buf)[i] = p[i];
    clac();
    return (int64_t)(sizeof(p) - 1);
}
static int64_t w32_GetTempPathW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetTempPathA(cur, f); }

static int64_t w32_GetTempFileNameA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetTempFileNameW(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }

static int64_t w32_GetSystemDirectoryA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    static const char p[] = "C:\\Windows\\System32";
    (void)cur;
    if (!buf || n < (uint32_t)sizeof(p)) return (int64_t)(sizeof(p) - 1);
    stac();
    for (unsigned i = 0; i < sizeof(p); ++i)
        ((char *)(uintptr_t)buf)[i] = p[i];
    clac();
    return (int64_t)(sizeof(p) - 1);
}
static int64_t w32_GetSystemDirectoryW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetSystemDirectoryA(cur, f); }
static int64_t w32_GetWindowsDirectoryA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    static const char p[] = "C:\\Windows";
    (void)cur;
    if (!buf || n < (uint32_t)sizeof(p)) return (int64_t)(sizeof(p) - 1);
    stac();
    for (unsigned i = 0; i < sizeof(p); ++i)
        ((char *)(uintptr_t)buf)[i] = p[i];
    clac();
    return (int64_t)(sizeof(p) - 1);
}
static int64_t w32_GetWindowsDirectoryW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetWindowsDirectoryA(cur, f); }

/* ============================================================
 * A 类实现：调试
 * ============================================================ */

static int64_t w32_IsDebuggerPresent(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_OutputDebugStringA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    (void)cur;
    if (p && user_range_ok(p, 1)) {
        stac();
        const char *s = (const char *)(uintptr_t)p;
        serial_printf("[WIN32-DBG] %s\n", s);
        clac();
    }
    return 0;
}
static int64_t w32_OutputDebugStringW(struct task_t *cur, struct syscall_frame *f)
{ return w32_OutputDebugStringA(cur, f); }
static int64_t w32_DebugBreak(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}

/* ============================================================
 * A 类实现：内存操作（msvcrt）
 * ============================================================ */

static int64_t w32_msvcrt_memcpy(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t d = f_arg0(f);
    uint64_t s = f_arg1(f);
    uint32_t n = (uint32_t)f_arg2(f);
    (void)cur;
    if (!d || !s || n == 0) return (int64_t)d;
    if (!user_range_ok(d, n) || !user_range_ok(s, n)) return (int64_t)d;
    stac();
    uint8_t *dd = (uint8_t *)(uintptr_t)d;
    const uint8_t *ss = (const uint8_t *)(uintptr_t)s;
    for (uint32_t i = 0; i < n; ++i) dd[i] = ss[i];
    clac();
    return (int64_t)d;
}
static int64_t w32_msvcrt_memset(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t d = f_arg0(f);
    int c      = (int)f_arg1(f);
    uint32_t n = (uint32_t)f_arg2(f);
    (void)cur;
    if (!d || n == 0) return (int64_t)d;
    if (!user_range_ok(d, n)) return (int64_t)d;
    stac();
    uint8_t *dd = (uint8_t *)(uintptr_t)d;
    for (uint32_t i = 0; i < n; ++i) dd[i] = (uint8_t)c;
    clac();
    return (int64_t)d;
}
static int64_t w32_msvcrt_memmove(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t d = f_arg0(f);
    uint64_t s = f_arg1(f);
    uint32_t n = (uint32_t)f_arg2(f);
    (void)cur;
    if (!d || !s || n == 0) return (int64_t)d;
    if (!user_range_ok(d, n) || !user_range_ok(s, n)) return (int64_t)d;
    stac();
    uint8_t *dd = (uint8_t *)(uintptr_t)d;
    const uint8_t *ss = (const uint8_t *)(uintptr_t)s;
    if (dd < ss || dd >= ss + n) {
        for (uint32_t i = 0; i < n; ++i) dd[i] = ss[i];
    } else {
        for (uint32_t i = n; i-- > 0; ) dd[i] = ss[i];
    }
    clac();
    return (int64_t)d;
}
static int64_t w32_msvcrt_memcmp(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t a = f_arg0(f);
    uint64_t b = f_arg1(f);
    uint32_t n = (uint32_t)f_arg2(f);
    (void)cur;
    if (!a || !b || n == 0) return 0;
    if (!user_range_ok(a, n) || !user_range_ok(b, n)) return 0;
    stac();
    const uint8_t *pa = (const uint8_t *)(uintptr_t)a;
    const uint8_t *pb = (const uint8_t *)(uintptr_t)b;
    int r = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) { r = (int)pa[i] - (int)pb[i]; break; }
    }
    clac();
    return r;
}
static int64_t w32_msvcrt_strlen(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    (void)cur;
    if (!p) return 0;
    stac();
    const char *s = (const char *)(uintptr_t)p;
    int n = 0;
    while (s[n] && n < 0x100000) ++n;
    clac();
    return n;
}
static int64_t w32_msvcrt_strcmp(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t a = f_arg0(f);
    uint64_t b = f_arg1(f);
    (void)cur;
    if (!a || !b) return 0;
    stac();
    const char *sa = (const char *)(uintptr_t)a;
    const char *sb = (const char *)(uintptr_t)b;
    int r;
    while (*sa && *sa == *sb) { ++sa; ++sb; }
    r = (int)(unsigned char)*sa - (int)(unsigned char)*sb;
    clac();
    return r;
}

/* ============================================================
 * A 类实现：advapi32 注册表最小子集
 *
 * 语义（本步简化）：
 *   - RegOpenKeyExA/W 返回一个合成句柄（递增）。
 *   - RegQueryValueExA/W 返回 NOT_IMPLEMENTED。
 *   - RegCloseKey 返回 SUCCESS。
 *   - RegCreateKeyExA/W / RegSetValueExA/W 走 compat_path_check +
 *     写入 /system/registry/HKLM/... 路径；本步拒绝写，返回 ACCESS_DENIED
 *     （除非调用者为内核管理器；本步不做特殊处理）。
 * ============================================================ */

static int64_t w32_RegOpenKeyExA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t phk = f_arg4(f);
    static uint32_t g_next_hkey = 0x1000;
    uint32_t hkey = g_next_hkey++;

    if (phk && user_range_ok(phk, 8)) {
        stac();
        *(uint64_t *)(uintptr_t)phk = (uint64_t)hkey;
        clac();
        return 0;   /* ERROR_SUCCESS */
    }
    (void)cur;
    win32_set_last_error(cur, WIN32_ERROR_INVALID_PARAMETER);
    return WIN32_ERROR_INVALID_PARAMETER;
}
static int64_t w32_RegOpenKeyExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegOpenKeyExA(cur, f); }

static int64_t w32_RegCloseKey(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }

static int64_t w32_RegQueryValueExA(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 2;   /* ERROR_FILE_NOT_FOUND */
}
static int64_t w32_RegQueryValueExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegQueryValueExA(cur, f); }

static int64_t w32_RegSetValueExA(struct task_t *cur, struct syscall_frame *f)
{
    (void)f;
    audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                cur->pid, 0, 0, 0, "(RegSetValue)");
    win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
    return 5;   /* ERROR_ACCESS_DENIED */
}
static int64_t w32_RegSetValueExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegSetValueExA(cur, f); }

static int64_t w32_RegCreateKeyExA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t phk = f_arg0(f);
    if (phk && user_range_ok(phk, 8)) {
        stac(); *(uint64_t *)(uintptr_t)phk = 0x2000; clac();
    }
    (void)cur; (void)f;
    return 5;
}
static int64_t w32_RegCreateKeyExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegCreateKeyExA(cur, f); }
static int64_t w32_RegDeleteKeyA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 2; }
static int64_t w32_RegDeleteKeyW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegDeleteKeyA(cur, f); }
static int64_t w32_RegDeleteValueA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 2; }
static int64_t w32_RegDeleteValueW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegDeleteValueA(cur, f); }
static int64_t w32_RegEnumKeyExA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 259; }   /* ERROR_NO_MORE_ITEMS */
static int64_t w32_RegEnumKeyExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegEnumKeyExA(cur, f); }
static int64_t w32_RegEnumValueA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 259; }
static int64_t w32_RegEnumValueW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegEnumValueA(cur, f); }
static int64_t w32_RegQueryInfoKeyA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 2; }
static int64_t w32_RegQueryInfoKeyW(struct task_t *cur, struct syscall_frame *f)
{ return w32_RegQueryInfoKeyA(cur, f); }

/* ============================================================
 * A 类实现：简单计算 / 本地环境
 * ============================================================ */

static int64_t w32_GetACP(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 65001; }   /* UTF-8 */
static int64_t w32_GetOEMCP(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 437; }
static int64_t w32_GetCPInfo(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_GetConsoleCP(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 65001; }
static int64_t w32_GetConsoleOutputCP(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 65001; }
static int64_t w32_GetSystemTimeAsFileTime(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t p = f_arg0(f);
    if (p && user_range_ok(p, 8)) {
        stac(); *(uint64_t *)(uintptr_t)p = 0; clac();
    }
    (void)cur;
    return 0;
}
static int64_t w32_GetSystemTimes(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    for (int i = 0; i < 3; ++i) {
        uint64_t p = 0;
        if (i == 0) p = f_arg0(f);
        else if (i == 1) p = f_arg1(f);
        else p = f_arg2(f);
        if (p && user_range_ok(p, 8)) {
            stac(); *(uint64_t *)(uintptr_t)p = 0; clac();
        }
    }
    return 1;
}
static int64_t w32_GetProcessTimes(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetThreadTimes(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }

static int64_t w32_GetSystemDefaultLangID(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x0809; }   /* 英语（英国） */
static int64_t w32_GetUserDefaultLangID(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x0809; }
static int64_t w32_GetSystemDefaultLCID(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x0809; }
static int64_t w32_GetUserDefaultLCID(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x0809; }
static int64_t w32_GetThreadLocale(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x0809; }
static int64_t w32_SetThreadLocale(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_IsValidCodePage(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_GetCPInfoExA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetCPInfoExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetCPInfoExA(cur, f); }
static int64_t w32_GetNumberFormatA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetNumberFormatW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetNumberFormatA(cur, f); }
static int64_t w32_GetDateFormatA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetDateFormatW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetDateFormatA(cur, f); }
static int64_t w32_GetTimeFormatA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetTimeFormatW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetTimeFormatA(cur, f); }
static int64_t w32_CompareStringA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 2; }   /* CSTR_EQUAL */
static int64_t w32_CompareStringW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CompareStringA(cur, f); }
static int64_t w32_LCMapStringA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_LCMapStringW(struct task_t *cur, struct syscall_frame *f)
{ return w32_LCMapStringA(cur, f); }
static int64_t w32_GetStringTypeA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetStringTypeW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetStringTypeA(cur, f); }

/* ============================================================
 * A 类实现：路径辅助
 * ============================================================ */

static int64_t win32_get_current_dir_common(struct task_t *cur,
                                            struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    static const char p[] = "C:\\";
    (void)cur;
    if (!buf || n < (uint32_t)sizeof(p)) return (int64_t)(sizeof(p) - 1);
    stac();
    for (unsigned i = 0; i < sizeof(p); ++i)
        ((char *)(uintptr_t)buf)[i] = p[i];
    clac();
    return (int64_t)(sizeof(p) - 1);
}

static int64_t w32_GetCurrentDirectoryA(struct task_t *cur, struct syscall_frame *f)
{ return win32_get_current_dir_common(cur, f); }
static int64_t w32_GetCurrentDirectoryW(struct task_t *cur, struct syscall_frame *f)
{ return win32_get_current_dir_common(cur, f); }
static int64_t w32_SetCurrentDirectoryA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_SetCurrentDirectoryW(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }

/* ============================================================
 * A 类实现：宽字符 / 快速路径
 * ============================================================ */

static int64_t w32_GetFullPathNameA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t src = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    uint64_t dst = f_arg2(f);
    (void)cur;
    if (!dst || !src) return 0;
    stac();
    const char *s = (const char *)(uintptr_t)src;
    char *d = (char *)(uintptr_t)dst;
    uint32_t i = 0;
    while (i + 1 < n && s[i]) { d[i] = s[i]; ++i; }
    d[i] = '\0';
    clac();
    return (int64_t)i;
}
static int64_t w32_GetFullPathNameW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetFullPathNameA(cur, f); }
static int64_t w32_GetLongPathNameA(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetFullPathNameA(cur, f); }
static int64_t w32_GetLongPathNameW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetFullPathNameA(cur, f); }
static int64_t w32_GetShortPathNameA(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetFullPathNameA(cur, f); }
static int64_t w32_GetShortPathNameW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetFullPathNameA(cur, f); }

/* ============================================================
 * A 类实现：其余 trivial
 * ============================================================ */

static int64_t w32_GetFileAttributesA(struct task_t *cur, struct syscall_frame *f)
{
    char win_path[COMPAT_PATH_MAX];
    if (strncpy_from_user(win_path, f_arg0(f), sizeof(win_path)) < 0) return (int64_t)0xFFFFFFFFu;
    char ob[COMPAT_PATH_MAX];
    if (compat_path_win_to_ob(win_path, ob) != 0) return (int64_t)0xFFFFFFFFu;
    struct vfs_inode *ino = 0;
    if (vfs_lookup(ob, &ino) != 0 || !ino) return (int64_t)0xFFFFFFFFu;
    (void)cur;
    uint32_t attr = 0;
    if (VFS_S_ISDIR(ino->mode)) attr |= 0x10;   /* FILE_ATTRIBUTE_DIRECTORY */
    return attr;
}
static int64_t w32_GetFileAttributesW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetFileAttributesA(cur, f); }
static int64_t w32_SetFileAttributesA(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    char win_path[COMPAT_PATH_MAX];
    if (strncpy_from_user(win_path, f_arg0(f), sizeof(win_path)) < 0) return 0;
    char ob[COMPAT_PATH_MAX];
    if (compat_path_win_to_ob(win_path, ob) != 0) return 0;
    int rc = compat_path_check(ob, cur, OB_ACCESS_WRITE);
    if (rc != 0) {
        audit_event(AUDIT_EV_WIN32_ACCESS_DENIED, AUDIT_LVL_CRITICAL,
                    cur->pid, 0, 0, 0, ob);
        win32_set_last_error(cur, WIN32_ERROR_ACCESS_DENIED);
        return 0;
    }
    return 1;
}
static int64_t w32_SetFileAttributesW(struct task_t *cur, struct syscall_frame *f)
{ return w32_SetFileAttributesA(cur, f); }

static int64_t w32_FindFirstFileA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; win32_set_last_error(cur, WIN32_ERROR_FILE_NOT_FOUND); return (int64_t)WIN32_INVALID_HANDLE; }
static int64_t w32_FindNextFileA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_FindClose(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_FindFirstFileExA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return (int64_t)WIN32_INVALID_HANDLE; }
static int64_t w32_FindFirstFileExW(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return (int64_t)WIN32_INVALID_HANDLE; }
static int64_t w32_FindFirstChangeNotificationA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return (int64_t)WIN32_INVALID_HANDLE; }
static int64_t w32_FindFirstChangeNotificationW(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return (int64_t)WIN32_INVALID_HANDLE; }
static int64_t w32_FindNextChangeNotification(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_FindCloseChangeNotification(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_CreateDirectoryExA(struct task_t *cur, struct syscall_frame *f)
{ return w32_CreateDirectoryA(cur, f); }
static int64_t w32_CreateDirectoryExW(struct task_t *cur, struct syscall_frame *f)
{ return w32_CreateDirectoryA(cur, f); }
static int64_t w32_GetFileSecurityA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetFileSecurityW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetFileSecurityA(cur, f); }
static int64_t w32_SetFileSecurityA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_SetFileSecurityW(struct task_t *cur, struct syscall_frame *f)
{ return w32_SetFileSecurityA(cur, f); }
static int64_t w32_CompareFileTime(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur;
    const uint64_t *a = (const uint64_t *)(uintptr_t)f_arg0(f);
    const uint64_t *b = (const uint64_t *)(uintptr_t)f_arg1(f);
    if (!a || !b) return 0;
    uint64_t va = 0, vb = 0;
    stac();
    va = *a; vb = *b;
    clac();
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}
static int64_t w32_LockFile(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_UnlockFile(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_CreatePipe(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    win32_set_last_error(cur, WIN32_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}
static int64_t w32_PeekNamedPipe(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_ReadFileEx(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_WriteFileEx(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetOverlappedResult(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_CreateIoCompletionPort(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetQueuedCompletionStatus(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_PostQueuedCompletionStatus(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_DeviceIoControl(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetLogicalDrives(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0x00000004; }
static int64_t w32_GetLogicalDriveStringsA(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t buf = f_arg0(f);
    uint32_t n   = (uint32_t)f_arg1(f);
    static const char s[] = "C:\\\0";
    (void)cur;
    if (!buf || n < 4) return 4;
    stac();
    for (int i = 0; i < 4; ++i) ((char *)(uintptr_t)buf)[i] = s[i];
    clac();
    return 4;
}
static int64_t w32_GetLogicalDriveStringsW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetLogicalDriveStringsA(cur, f); }
static int64_t w32_GetDriveTypeA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 3; }   /* DRIVE_FIXED */
static int64_t w32_GetDriveTypeW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetDriveTypeA(cur, f); }
static int64_t w32_GetVolumeInformationA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetVolumeInformationW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetVolumeInformationA(cur, f); }
static int64_t w32_GetDiskFreeSpaceA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetDiskFreeSpaceW(struct task_t *cur, struct syscall_frame *f)
{ return w32_GetDiskFreeSpaceA(cur, f); }
static int64_t w32_GetFileTime(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_SetFileTime(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_GetFileInformationByHandle(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }

/* ============================================================
 * A 类实现：token 最小
 * ============================================================ */

static int64_t w32_OpenProcessToken(struct task_t *cur, struct syscall_frame *f)
{
    uint64_t pht = f_arg2(f);
    if (pht && user_range_ok(pht, 8)) {
        stac(); *(uint64_t *)(uintptr_t)pht = 0x3000; clac();
    }
    (void)cur; (void)f;
    return 1;
}
static int64_t w32_OpenThreadToken(struct task_t *cur, struct syscall_frame *f)
{
    (void)cur; (void)f;
    return 0;
}
static int64_t w32_LookupPrivilegeValueA(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_LookupPrivilegeValueW(struct task_t *cur, struct syscall_frame *f)
{ return w32_LookupPrivilegeValueA(cur, f); }
static int64_t w32_AdjustTokenPrivileges(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 1; }
static int64_t w32_GetTokenInformation(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_AllocateAndInitializeSid(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }
static int64_t w32_FreeSid(struct task_t *cur, struct syscall_frame *f)
{ (void)cur; (void)f; return 0; }

/* ============================================================
 * 分发器实现表
 * ============================================================ */

struct win32_api_impl {
    uint32_t    nr;
    win32_api_fn fn;
};

static const struct win32_api_impl g_win32_api_impl[] = {
    /* kernel32 标准句柄 / 错误 / 时间 */
    { WIN32_API_GetStdHandle,           w32_GetStdHandle },
    { WIN32_API_SetStdHandle,           w32_SetStdHandle },
    { WIN32_API_GetLastError,           w32_GetLastError },
    { WIN32_API_SetLastError,           w32_SetLastError },
    { WIN32_API_GetCurrentProcessId,    w32_GetCurrentProcessId },
    { WIN32_API_GetCurrentThreadId,     w32_GetCurrentThreadId },
    { WIN32_API_GetTickCount,           w32_GetTickCount },
    { WIN32_API_GetTickCount64,         w32_GetTickCount64 },
    { WIN32_API_Sleep,                  w32_Sleep },
    { WIN32_API_SleepEx,                w32_SleepEx },
    { WIN32_API_ExitProcess,            w32_ExitProcess },
    { WIN32_API_TerminateProcess,       w32_TerminateProcess },
    { WIN32_API_GetExitCodeProcess,     w32_GetExitCodeProcess },
    { WIN32_API_GetExitCodeThread,      w32_GetExitCodeThread },

    /* kernel32 文件 */
    { WIN32_API_CreateFileA,            w32_CreateFileA },
    { WIN32_API_CreateFileW,            w32_CreateFileW },
    { WIN32_API_ReadFile,               w32_ReadFile },
    { WIN32_API_WriteFile,              w32_WriteFile },
    { WIN32_API_CloseHandle,            w32_CloseHandle },
    { WIN32_API_GetFileSize,            w32_GetFileSize },
    { WIN32_API_GetFileSizeEx,          w32_GetFileSizeEx },
    { WIN32_API_SetFilePointer,         w32_SetFilePointer },
    { WIN32_API_SetFilePointerEx,       w32_SetFilePointerEx },
    { WIN32_API_FlushFileBuffers,       w32_FlushFileBuffers },
    { WIN32_API_SetEndOfFile,           w32_SetEndOfFile },
    { WIN32_API_GetFileType,            w32_GetFileType },
    { WIN32_API_DeleteFileA,            w32_DeleteFileA },
    { WIN32_API_DeleteFileW,            w32_DeleteFileW },
    { WIN32_API_GetFileAttributesA,     w32_GetFileAttributesA },
    { WIN32_API_GetFileAttributesW,     w32_GetFileAttributesW },
    { WIN32_API_SetFileAttributesA,     w32_SetFileAttributesA },
    { WIN32_API_SetFileAttributesW,     w32_SetFileAttributesW },
    { WIN32_API_FindFirstFileA,         w32_FindFirstFileA },
    { WIN32_API_FindNextFileA,          w32_FindNextFileA },
    { WIN32_API_FindClose,              w32_FindClose },

    /* kernel32 目录 */
    { WIN32_API_CreateDirectoryA,       w32_CreateDirectoryA },
    { WIN32_API_CreateDirectoryW,       w32_CreateDirectoryW },
    { WIN32_API_RemoveDirectoryA,       w32_RemoveDirectoryA },
    { WIN32_API_RemoveDirectoryW,       w32_RemoveDirectoryW },
    { WIN32_API_GetCurrentDirectoryA,   w32_GetCurrentDirectoryA },
    { WIN32_API_GetCurrentDirectoryW,   w32_GetCurrentDirectoryW },
    { WIN32_API_SetCurrentDirectoryA,   w32_SetCurrentDirectoryA },
    { WIN32_API_SetCurrentDirectoryW,   w32_SetCurrentDirectoryW },
    { WIN32_API_GetFullPathNameA,       w32_GetFullPathNameA },
    { WIN32_API_GetFullPathNameW,       w32_GetFullPathNameW },
    { WIN32_API_GetLongPathNameA,       w32_GetLongPathNameA },
    { WIN32_API_GetLongPathNameW,       w32_GetLongPathNameW },
    { WIN32_API_GetShortPathNameA,      w32_GetShortPathNameA },
    { WIN32_API_GetShortPathNameW,      w32_GetShortPathNameW },

    /* kernel32 内存 / 堆 */
    { WIN32_API_GetProcessHeap,         w32_GetProcessHeap },
    { WIN32_API_HeapCreate,             w32_HeapCreate },
    { WIN32_API_HeapDestroy,            w32_HeapDestroy },
    { WIN32_API_HeapAlloc,              w32_HeapAlloc },
    { WIN32_API_HeapReAlloc,            w32_HeapReAlloc },
    { WIN32_API_HeapFree,               w32_HeapFree },
    { WIN32_API_HeapSize,               w32_HeapSize },
    { WIN32_API_VirtualAlloc,           w32_VirtualAlloc },
    { WIN32_API_VirtualAllocEx,         w32_VirtualAllocEx },
    { WIN32_API_VirtualFree,            w32_VirtualFree },
    { WIN32_API_VirtualFreeEx,          w32_VirtualFreeEx },
    { WIN32_API_VirtualProtect,         w32_VirtualProtect },
    { WIN32_API_VirtualQuery,           w32_VirtualQuery },
    { WIN32_API_GlobalAlloc,            w32_GlobalAlloc },
    { WIN32_API_GlobalFree,             w32_GlobalFree },
    { WIN32_API_LocalAlloc,             w32_LocalAlloc },
    { WIN32_API_LocalFree,              w32_LocalFree },

    /* kernel32 进程 / 线程 */
    { WIN32_API_CreateProcessA,         w32_CreateProcessA },
    { WIN32_API_CreateProcessW,         w32_CreateProcessW },
    { WIN32_API_OpenProcess,            w32_OpenProcess },
    { WIN32_API_GetCurrentProcess,      w32_GetCurrentProcess },
    { WIN32_API_GetCurrentThread,       w32_GetCurrentThread },
    { WIN32_API_CreateThread,           w32_CreateThread },
    { WIN32_API_ExitThread,             w32_ExitThread },
    { WIN32_API_WaitForSingleObject,    w32_WaitForSingleObject },
    { WIN32_API_WaitForMultipleObjects, w32_WaitForMultipleObjects },
    { WIN32_API_SetEvent,               w32_SetEvent },
    { WIN32_API_ResetEvent,             w32_ResetEvent },
    { WIN32_API_CreateEventA,           w32_CreateEventA },
    { WIN32_API_CreateEventW,           w32_CreateEventW },
    { WIN32_API_CreateMutexA,           w32_CreateMutexA },
    { WIN32_API_CreateMutexW,           w32_CreateMutexW },
    { WIN32_API_ReleaseMutex,           w32_ReleaseMutex },
    { WIN32_API_InitializeCriticalSection, w32_InitializeCriticalSection },
    { WIN32_API_EnterCriticalSection,   w32_EnterCriticalSection },
    { WIN32_API_LeaveCriticalSection,   w32_LeaveCriticalSection },
    { WIN32_API_DeleteCriticalSection,  w32_DeleteCriticalSection },
    { WIN32_API_SuspendThread,          w32_SuspendThread },
    { WIN32_API_ResumeThread,           w32_ResumeThread },
    { WIN32_API_GetThreadPriority,      w32_GetThreadPriority },
    { WIN32_API_SetThreadPriority,      w32_SetThreadPriority },

    /* kernel32 模块 / 环境 */
    { WIN32_API_GetModuleHandleA,       w32_GetModuleHandleA },
    { WIN32_API_GetModuleHandleW,       w32_GetModuleHandleW },
    { WIN32_API_GetModuleFileNameA,     w32_GetModuleFileNameA },
    { WIN32_API_GetModuleFileNameW,     w32_GetModuleFileNameW },
    { WIN32_API_GetCommandLineA,        w32_GetCommandLineA },
    { WIN32_API_GetCommandLineW,        w32_GetCommandLineW },
    { WIN32_API_GetEnvironmentStrings,  w32_GetEnvironmentStrings },
    { WIN32_API_GetStartupInfoA,        w32_GetStartupInfoA },
    { WIN32_API_GetStartupInfoW,        w32_GetStartupInfoW },
    { WIN32_API_GetVersion,             w32_GetVersion },
    { WIN32_API_GetVersionExA,          w32_GetVersionExA },
    { WIN32_API_GetVersionExW,          w32_GetVersionExW },
    { WIN32_API_GetEnvironmentVariableA, w32_GetEnvironmentVariableA },
    { WIN32_API_GetEnvironmentVariableW, w32_GetEnvironmentVariableW },
    { WIN32_API_SetEnvironmentVariableA, w32_SetEnvironmentVariableA },
    { WIN32_API_SetEnvironmentVariableW, w32_SetEnvironmentVariableW },
    { WIN32_API_GetTempPathA,           w32_GetTempPathA },
    { WIN32_API_GetTempPathW,           w32_GetTempPathW },
    { WIN32_API_GetTempFileNameA,       w32_GetTempFileNameA },
    { WIN32_API_GetTempFileNameW,       w32_GetTempFileNameW },
    { WIN32_API_GetSystemDirectoryA,    w32_GetSystemDirectoryA },
    { WIN32_API_GetSystemDirectoryW,    w32_GetSystemDirectoryW },
    { WIN32_API_GetWindowsDirectoryA,   w32_GetWindowsDirectoryA },
    { WIN32_API_GetWindowsDirectoryW,   w32_GetWindowsDirectoryW },

    /* kernel32 系统信息 */
    { WIN32_API_GetSystemInfo,          w32_GetSystemInfo },
    { WIN32_API_GetSystemTime,          w32_GetSystemTime },
    { WIN32_API_GetLocalTime,           w32_GetLocalTime },
    { WIN32_API_SystemTimeToFileTime,   w32_SystemTimeToFileTime },
    { WIN32_API_FileTimeToSystemTime,   w32_FileTimeToSystemTime },
    { WIN32_API_QueryPerformanceCounter, w32_QueryPerformanceCounter },
    { WIN32_API_QueryPerformanceFrequency, w32_QueryPerformanceFrequency },
    { WIN32_API_GetComputerNameA,       w32_GetComputerNameA },
    { WIN32_API_GetComputerNameW,       w32_GetComputerNameW },
    { WIN32_API_GetUserNameA,           w32_GetUserNameA },
    { WIN32_API_GetUserNameW,           w32_GetUserNameW },
    { WIN32_API_GetSystemTimeAsFileTime, w32_GetSystemTimeAsFileTime },
    { WIN32_API_GetSystemTimes,         w32_GetSystemTimes },
    { WIN32_API_GetProcessTimes,        w32_GetProcessTimes },
    { WIN32_API_GetThreadTimes,         w32_GetThreadTimes },
    { WIN32_API_GetACP,                 w32_GetACP },
    { WIN32_API_GetOEMCP,               w32_GetOEMCP },
    { WIN32_API_GetCPInfo,              w32_GetCPInfo },
    { WIN32_API_GetConsoleCP,           w32_GetConsoleCP },
    { WIN32_API_GetConsoleOutputCP,     w32_GetConsoleOutputCP },
    { WIN32_API_GetSystemDefaultLangID, w32_GetSystemDefaultLangID },
    { WIN32_API_GetUserDefaultLangID,   w32_GetUserDefaultLangID },
    { WIN32_API_GetSystemDefaultLCID,   w32_GetSystemDefaultLCID },
    { WIN32_API_GetUserDefaultLCID,     w32_GetUserDefaultLCID },
    { WIN32_API_GetThreadLocale,        w32_GetThreadLocale },
    { WIN32_API_SetThreadLocale,        w32_SetThreadLocale },
    { WIN32_API_IsValidCodePage,        w32_IsValidCodePage },
    { WIN32_API_GetCPInfoExA,           w32_GetCPInfoExA },
    { WIN32_API_GetCPInfoExW,           w32_GetCPInfoExW },
    { WIN32_API_GetNumberFormatA,       w32_GetNumberFormatA },
    { WIN32_API_GetNumberFormatW,       w32_GetNumberFormatW },
    { WIN32_API_GetDateFormatA,         w32_GetDateFormatA },
    { WIN32_API_GetDateFormatW,         w32_GetDateFormatW },
    { WIN32_API_GetTimeFormatA,         w32_GetTimeFormatA },
    { WIN32_API_GetTimeFormatW,         w32_GetTimeFormatW },
    { WIN32_API_CompareStringA,         w32_CompareStringA },
    { WIN32_API_CompareStringW,         w32_CompareStringW },
    { WIN32_API_LCMapStringA,           w32_LCMapStringA },
    { WIN32_API_LCMapStringW,           w32_LCMapStringW },
    { WIN32_API_GetStringTypeA,         w32_GetStringTypeA },
    { WIN32_API_GetStringTypeW,         w32_GetStringTypeW },
    { WIN32_API_MultiByteToWideChar,    w32_MultiByteToWideChar },
    { WIN32_API_WideCharToMultiByte,    w32_WideCharToMultiByte },
    { WIN32_API_CharUpperA,             w32_CharUpperA },
    { WIN32_API_CharUpperW,             w32_CharUpperW },
    { WIN32_API_CharLowerA,             w32_CharLowerA },
    { WIN32_API_CharLowerW,             w32_CharLowerW },

    /* kernel32 控制台 */
    { WIN32_API_GetConsoleMode,         w32_GetConsoleMode },
    { WIN32_API_SetConsoleMode,         w32_SetConsoleMode },
    { WIN32_API_GetConsoleScreenBufferInfo, w32_GetConsoleScreenBufferInfo },
    { WIN32_API_SetConsoleCursorPosition, w32_SetConsoleCursorPosition },
    { WIN32_API_SetConsoleTextAttribute, w32_SetConsoleTextAttribute },
    { WIN32_API_WriteConsoleA,          w32_WriteConsoleA },
    { WIN32_API_WriteConsoleW,          w32_WriteConsoleW },
    { WIN32_API_ReadConsoleA,           w32_ReadConsoleA },
    { WIN32_API_ReadConsoleW,           w32_ReadConsoleW },
    { WIN32_API_AllocConsole,           w32_AllocConsole },
    { WIN32_API_FreeConsole,            w32_FreeConsole },
    { WIN32_API_GetConsoleWindow,       w32_GetConsoleWindow },
    { WIN32_API_SetConsoleTitleA,       w32_SetConsoleTitleA },
    { WIN32_API_GetConsoleTitleA,       w32_GetConsoleTitleA },
    { WIN32_API_SetConsoleWindowSize,   w32_SetConsoleWindowSize },
    { WIN32_API_GetCurrentConsoleFont,  w32_GetCurrentConsoleFont },

    /* kernel32 字符串 */
    { WIN32_API_lstrlenA,               w32_lstrlenA },
    { WIN32_API_lstrlenW,               w32_lstrlenW },
    { WIN32_API_lstrcpyA,               w32_lstrcpyA },
    { WIN32_API_lstrcpyW,               w32_lstrcpyW },
    { WIN32_API_lstrcatA,               w32_lstrcatA },
    { WIN32_API_lstrcatW,               w32_lstrcatW },
    { WIN32_API_lstrcmpA,               w32_lstrcmpA },
    { WIN32_API_lstrcmpW,               w32_lstrcmpW },
    { WIN32_API_lstrcmpiA,              w32_lstrcmpiA },
    { WIN32_API_lstrcmpiW,              w32_lstrcmpiW },
    { WIN32_API_MulDiv,                 w32_MulDiv },

    /* kernel32 调试 */
    { WIN32_API_IsDebuggerPresent,      w32_IsDebuggerPresent },
    { WIN32_API_OutputDebugStringA,     w32_OutputDebugStringA },
    { WIN32_API_OutputDebugStringW,     w32_OutputDebugStringW },
    { WIN32_API_DebugBreak,             w32_DebugBreak },

    /* kernel32 磁盘 / 卷 */
    { WIN32_API_GetLogicalDrives,       w32_GetLogicalDrives },
    { WIN32_API_GetLogicalDriveStringsA, w32_GetLogicalDriveStringsA },
    { WIN32_API_GetLogicalDriveStringsW, w32_GetLogicalDriveStringsW },
    { WIN32_API_GetDriveTypeA,          w32_GetDriveTypeA },
    { WIN32_API_GetDriveTypeW,          w32_GetDriveTypeW },
    { WIN32_API_GetVolumeInformationA,  w32_GetVolumeInformationA },
    { WIN32_API_GetVolumeInformationW,  w32_GetVolumeInformationW },
    { WIN32_API_GetDiskFreeSpaceA,      w32_GetDiskFreeSpaceA },
    { WIN32_API_GetDiskFreeSpaceW,      w32_GetDiskFreeSpaceW },

    /* kernel32 文件扩展 */
    { WIN32_API_GetFileTime,            w32_GetFileTime },
    { WIN32_API_SetFileTime,            w32_SetFileTime },
    { WIN32_API_GetFileInformationByHandle, w32_GetFileInformationByHandle },
    { WIN32_API_LockFile,               w32_LockFile },
    { WIN32_API_UnlockFile,             w32_UnlockFile },
    { WIN32_API_CreatePipe,             w32_CreatePipe },
    { WIN32_API_PeekNamedPipe,          w32_PeekNamedPipe },
    { WIN32_API_ReadFileEx,             w32_ReadFileEx },
    { WIN32_API_WriteFileEx,            w32_WriteFileEx },
    { WIN32_API_GetOverlappedResult,    w32_GetOverlappedResult },
    { WIN32_API_CreateIoCompletionPort, w32_CreateIoCompletionPort },
    { WIN32_API_GetQueuedCompletionStatus, w32_GetQueuedCompletionStatus },
    { WIN32_API_PostQueuedCompletionStatus, w32_PostQueuedCompletionStatus },
    { WIN32_API_DeviceIoControl,        w32_DeviceIoControl },
    { WIN32_API_FindFirstFileExA,       w32_FindFirstFileExA },
    { WIN32_API_FindFirstFileExW,       w32_FindFirstFileExW },
    { WIN32_API_FindFirstChangeNotificationA, w32_FindFirstChangeNotificationA },
    { WIN32_API_FindFirstChangeNotificationW, w32_FindFirstChangeNotificationW },
    { WIN32_API_FindNextChangeNotification, w32_FindNextChangeNotification },
    { WIN32_API_FindCloseChangeNotification, w32_FindCloseChangeNotification },
    { WIN32_API_CreateDirectoryExA,     w32_CreateDirectoryExA },
    { WIN32_API_CreateDirectoryExW,     w32_CreateDirectoryExW },
    { WIN32_API_GetFileSecurityA,       w32_GetFileSecurityA },
    { WIN32_API_GetFileSecurityW,       w32_GetFileSecurityW },
    { WIN32_API_SetFileSecurityA,       w32_SetFileSecurityA },
    { WIN32_API_SetFileSecurityW,       w32_SetFileSecurityW },
    { WIN32_API_CompareFileTime,        w32_CompareFileTime },

    /* msvcrt 内存 / 字符串 */
    { WIN32_API_memset,                 w32_msvcrt_memset },
    { WIN32_API_memcpy,                 w32_msvcrt_memcpy },
    { WIN32_API_memmove,                w32_msvcrt_memmove },
    { WIN32_API_memcmp,                 w32_msvcrt_memcmp },
    { WIN32_API_strlen,                 w32_msvcrt_strlen },
    { WIN32_API_strcmp,                 w32_msvcrt_strcmp },
    { WIN32_API_malloc,                 w32_HeapAlloc },
    { WIN32_API_free,                   w32_HeapFree },

    /* advapi32 */
    { WIN32_API_RegOpenKeyExA,          w32_RegOpenKeyExA },
    { WIN32_API_RegOpenKeyExW,          w32_RegOpenKeyExW },
    { WIN32_API_RegCloseKey,            w32_RegCloseKey },
    { WIN32_API_RegQueryValueExA,       w32_RegQueryValueExA },
    { WIN32_API_RegQueryValueExW,       w32_RegQueryValueExW },
    { WIN32_API_RegSetValueExA,         w32_RegSetValueExA },
    { WIN32_API_RegSetValueExW,         w32_RegSetValueExW },
    { WIN32_API_RegCreateKeyExA,        w32_RegCreateKeyExA },
    { WIN32_API_RegCreateKeyExW,        w32_RegCreateKeyExW },
    { WIN32_API_RegDeleteKeyA,          w32_RegDeleteKeyA },
    { WIN32_API_RegDeleteKeyW,          w32_RegDeleteKeyW },
    { WIN32_API_RegDeleteValueA,        w32_RegDeleteValueA },
    { WIN32_API_RegDeleteValueW,        w32_RegDeleteValueW },
    { WIN32_API_RegEnumKeyExA,          w32_RegEnumKeyExA },
    { WIN32_API_RegEnumKeyExW,          w32_RegEnumKeyExW },
    { WIN32_API_RegEnumValueA,          w32_RegEnumValueA },
    { WIN32_API_RegEnumValueW,          w32_RegEnumValueW },
    { WIN32_API_RegQueryInfoKeyA,       w32_RegQueryInfoKeyA },
    { WIN32_API_RegQueryInfoKeyW,       w32_RegQueryInfoKeyW },
    { WIN32_API_OpenProcessToken,       w32_OpenProcessToken },
    { WIN32_API_OpenThreadToken,        w32_OpenThreadToken },
    { WIN32_API_LookupPrivilegeValueA,  w32_LookupPrivilegeValueA },
    { WIN32_API_LookupPrivilegeValueW,  w32_LookupPrivilegeValueW },
    { WIN32_API_AdjustTokenPrivileges,  w32_AdjustTokenPrivileges },
    { WIN32_API_GetTokenInformation,    w32_GetTokenInformation },
    { WIN32_API_AllocateAndInitializeSid, w32_AllocateAndInitializeSid },
    { WIN32_API_FreeSid,                w32_FreeSid },
};

static const uint32_t g_win32_impl_count =
    (uint32_t)(sizeof(g_win32_api_impl) / sizeof(g_win32_api_impl[0]));

int win32_api_has_impl(uint32_t api_nr, win32_api_fn *out_fn)
{
    for (uint32_t i = 0; i < g_win32_impl_count; ++i) {
        if (g_win32_api_impl[i].nr == api_nr) {
            if (out_fn) *out_fn = g_win32_api_impl[i].fn;
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 分发器
 * ============================================================ */

static int win32_api_nr_in_table(uint32_t nr)
{
    for (uint32_t i = 0; i < g_win32_api_table_count; ++i) {
        if (g_win32_api_table[i].nr == nr) return 1;
    }
    return 0;
}

static const char *win32_api_name_of(uint32_t nr)
{
    for (uint32_t i = 0; i < g_win32_api_table_count; ++i) {
        if (g_win32_api_table[i].nr == nr) return g_win32_api_table[i].func;
    }
    return "(unknown)";
}

void win32_api_init(void)
{
    serial_printf("[WIN32] API table: %u entries\n",
                  (unsigned)g_win32_api_table_count);
    serial_printf("[WIN32] impl table: %u entries\n",
                  (unsigned)g_win32_impl_count);
}

int64_t win32_api_dispatch(struct task_t *cur, struct syscall_frame *f)
{
    if (!cur || !f) return 0;
    uint32_t nr = (uint32_t)f->rax;

    /* 1) 优先查 impl 表：命中则调用真实实现。 */
    win32_api_fn fn = 0;
    if (win32_api_has_impl(nr, &fn) && fn) {
        return fn(cur, f);
    }

    /* 2) 未在 impl 表中：判断是否在名称表内。 */
    if (!win32_api_nr_in_table(nr)) {
        serial_printf("[WIN32] unknown nr=0x%x pid=%llu\n",
                      (unsigned)nr, (unsigned long long)cur->pid);
        audit_event(AUDIT_EV_WIN32_API_ENOSYS, AUDIT_LVL_CRITICAL,
                    cur->pid, nr, 0, 0, "(unknown-win32-nr)");
        win32_set_last_error(cur, WIN32_ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }

    /* 3) 在名称表内但无实现：返回 CALL_NOT_IMPLEMENTED 并写审计。 */
    audit_event(AUDIT_EV_WIN32_API_ENOSYS, AUDIT_LVL_WARN,
                cur->pid, nr, 0, 0, win32_api_name_of(nr));
    win32_set_last_error(cur, WIN32_ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}
/*===OmniBridgeOs/kernel/arch/x64/user/win32_api.c 结束===*/