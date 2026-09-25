/*===OmniBridgeOs/kernel/arch/x64/user/user.h===*/
#ifndef OMNIBRIDGE_USER_USER_H
#define OMNIBRIDGE_USER_USER_H

#include <stdint.h>
#include "task.h"

#ifndef OB_EFAULT
#define OB_EFAULT  (-14)
#endif
#ifndef OB_EPERM
#define OB_EPERM   (-1)
#endif
#ifndef OB_EINVAL
#define OB_EINVAL  (-22)
#endif
#ifndef OB_ENOMEM
#define OB_ENOMEM  (-12)
#endif

#define USER_STACK_TOP      0x00007FFFFFFF0000ULL
#define USER_STACK_SIZE     0x0000000000010000ULL   /* 64 KB */
#define USER_STACK_BOTTOM   (USER_STACK_TOP - USER_STACK_SIZE)

#define USER_STACK_PAGES    16u                     /* 64 KB = 16 页 */

#define USER_TLS_STRIDE     0x1000ULL
#define USER_TLS_BASE       0x00007FFFFE000000ULL

#define USER_IMAGE_BASE     0x0000008000000000ULL   /* 512 GiB */
#define USER_IMAGE_MAX      (256ULL * 1024 * 1024)  /* 256 MiB */

#define USER_SIGTRAMP_ADDR  0x00007FFFFFFFE000ULL

struct user_regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

#define USER_THREAD_MAX_OWN_PAGES  USER_STACK_PAGES

struct user_ctx {
    uint64_t entry;
    uint64_t stack_top;
    uint64_t stack_bottom;
    uint64_t stack_phys[USER_STACK_PAGES];
    uint64_t tls_base;
    uint64_t tls_phys;
    uint64_t *pml4;
    uint64_t heap_cur;
    uint64_t heap_end;
    uint64_t heap_phys[256];
    uint32_t heap_page_count;
    uint32_t _pad;
    uint32_t owns_pml4;
    uint32_t is_thread;
    uint64_t thread_stack_base;
    uint64_t thread_stack_pages;
    uint64_t thread_tls_phys;

    /* ★ 任务 4 新增：本 pthread 线程"独占"的栈页物理地址。
     *   - 通过 ob_brk 复用父进程 heap 映射时不记录在此，teardown 不释放；
     *   - 只有真正 pmm_alloc_pages 分配的页才记入此数组，
     *     由 user_teardown 释放。
     *   容量等于 USER_STACK_PAGES，纯栈上数据，无动态分配。 */
    uint64_t thread_stack_owned_phys[USER_THREAD_MAX_OWN_PAGES];
    uint32_t thread_stack_owned_count;
    uint32_t _pad2;
};

/* ---------- 生命周期 ---------- */
void user_init(void);
void user_enter(uint64_t entry, uint64_t user_rsp,
                int argc, const char **argv);
int  user_setup(struct task_t *t, uint64_t entry_va);
void user_teardown(struct task_t *t);
struct user_ctx *user_get_ctx(struct task_t *t);

/* ★ 修复：旧接口保留（内部转调 user_resume_ctx） */
void user_resume(struct task_t *t);

/* ★ 新增：使用指定上下文恢复用户态。
 *   idt.c 使用 signal_handler_slot(cur) 进入 handler；
 *   syscall.c 使用 signal_resume_slot(cur) 从 sigreturn 恢复。 */
void user_resume_ctx(struct task_t *t, struct user_regs *r);

int  user_range_ok(uint64_t vaddr, uint64_t size);
void user_set_fsbase(uint64_t base);
uint64_t user_get_fsbase(void);

/* ---------- 用户栈写入 ---------- */
int user_stack_write(struct user_ctx *c, uint64_t uva,
                     const void *src, uint64_t len);

/* ---------- 用户态进程启动 ---------- */
int user_spawn_program(const char *path);
void user_launcher_entry(void *arg);

/* ---------- 用户态内存访问辅助 ---------- */
int copy_from_user(void *dst, uint64_t src_uaddr, uint64_t len);
int copy_to_user(uint64_t dst_uaddr, const void *src, uint64_t len);
int strnlen_user(uint64_t uaddr, uint64_t max);
int strncpy_from_user(char *dst, uint64_t uaddr, uint64_t max);

/* ---------- pthread 线程支撑 ---------- */
int64_t user_thread_spawn(struct task_t *parent,
                          uint64_t entry, uint64_t arg,
                          uint64_t ustack_top);

/* 信号 trampoline 固定地址。 */
#define USER_SIGTRAMP_ADDR  0x00007FFFFFFFE000ULL

/*
 * ★ 任务 4 新增：pthread 线程退出 trampoline 地址。
 *
 *   pthread 线程的用户函数返回时，ret 会从栈上弹出此地址作为 RIP。
 *   该地址处是一段 stub，把函数返回的 RAX（int 返回值）作为
 *   exit_code 传给 SYS_OB_UserExit，从而优雅终止线程。
 *
 *   位置：USER_SIGTRAMP_ADDR 页内 offset 0x20，不与信号 trampoline
 *   （offset 0x00）冲突。
 */
#define USER_THREAD_EXIT_TRAMP_ADDR  (USER_SIGTRAMP_ADDR + 0x20ULL)

/* ★ 第 19 步：Linux ELF64 入口。
 *
 * 与 user_spawn_program 类似，但期望 .elf 静态链接文件。
 * - 设置 compat_type = COMPAT_TYPE_LINUX；
 * - 使用 elf64_load_into_task 加载段；
 * - 使用 linux_build_stack 构建 Linux 栈；
 * - 通过 user_enter(entry, rsp, argc=-1, NULL) 进入 CPL=3。 */
int user_spawn_linux_elf(const char *path);

/* ★ 第 19 步：真 fork —— 复制父进程用户地址空间。
 * 内部完成 PML4[1..255] 用户子树的 deep copy。 */
int user_fork_address_space(struct task_t *parent, struct task_t *child);

/* ★ 第 20 步：GS 段寄存器基址（MSR 0xC0000101） */
void     user_set_gsbase(uint64_t base);
uint64_t user_get_gsbase(void);

/* ★ 第 20 步：PE 兼容入口。 */
int user_spawn_win32_pe(const char *path);

#endif /* OMNIBRIDGE_USER_USER_H */
/*===OmniBridgeOs/kernel/arch/x64/user/user.h 结束===*/