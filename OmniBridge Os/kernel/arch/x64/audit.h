/*===OmniBridgeOs/kernel/arch/x64/audit.h===*/
/*
 * 内核环形审计缓冲区。
 *
 * 第 18 步新增：兼容层相关事件类型。
 * ★ 第 18A 步新增：网络事件类型（ACCESS / DENIED / SANDBOX_BLOCK）。
 */
#ifndef OMNIBRIDGE_AUDIT_H
#define OMNIBRIDGE_AUDIT_H

#include <stdint.h>

/* ---------- 事件类型 ---------- */
#define AUDIT_EV_PID_VIOLATION        0u
#define AUDIT_EV_KERNEL_MEM_WRITE     1u
#define AUDIT_EV_KERNEL_MEM_READ      2u
#define AUDIT_EV_CRITICAL_ACCESS      3u
#define AUDIT_EV_SANDBOX_ESCAPE       4u
#define AUDIT_EV_ITA_FAILURE          5u
/* ★ 第 18 步：兼容层 */
#define AUDIT_EV_COMPAT_PATH_REDIRECT 6u
#define AUDIT_EV_COMPAT_ACCESS_DENIED 7u
#define AUDIT_EV_COMPAT_EXCEPTION     8u
/* ★ 第 18A 步：网络层
 *
 *   AUDIT_EV_NET_ACCESS          —— 普通网络访问通过（INFO 级）
 *   AUDIT_EV_NET_DENIED          —— 原生权限检查拒绝（CRITICAL 级）
 *   AUDIT_EV_NET_SANDBOX_BLOCK   —— 沙盒进程网络操作被拦截（CRITICAL 级）
 *
 * 参数约定（供 type_name/audit_event 打印）：
 *   arg0    —— 协议号、domain 或目标 IP（由调用方决定）
 *   arg1    —— 目标端口、socket type 或缓冲区地址
 *   mode    —— 保留，调用方传 0
 *   path    —— 简短描述字符串（如 "(socket)"、"(connect)"）
 */
/* ============================================================
 * kernel/arch/x64/audit.h  （仅替换以下区段）
 *
 * 修改：在 AUDIT_EV_NET_SANDBOX_BLOCK 之后追加 3 个 19 步新增事件，
 *       并更新 AUDIT_EV_COUNT 为 15。
 * ============================================================ */
#define AUDIT_EV_NET_ACCESS           9u
#define AUDIT_EV_NET_DENIED          10u
#define AUDIT_EV_NET_SANDBOX_BLOCK   11u

/* ★ 第 19 步：Linux ELF 兼容层事件。
 *
 * 人工必须审查：
 *   - COMPAT_SYSCALL 用于审计“兼容进程发起的 Linux syscall 分发”。
 *     级别由调用者决定（通常 INFO）；便于事后追踪兼容行为。
 *   - COMPAT_SYSCALL_ENOSYS 用于审计“未实现的 Linux syscall”，
 *     级别 CRITICAL；便于后续按频率补全。
 *   - COMPAT_FORK 用于审计 fork/clone 请求，级别 WARN。 */
#define AUDIT_EV_COMPAT_SYSCALL        12u
#define AUDIT_EV_COMPAT_SYSCALL_ENOSYS 13u
#define AUDIT_EV_COMPAT_FORK           14u

/* ★ 第 20 步：Windows PE 兼容层 */
#define AUDIT_EV_WIN32_API              15u
#define AUDIT_EV_WIN32_API_ENOSYS       16u
#define AUDIT_EV_WIN32_ACCESS_DENIED    17u
#define AUDIT_EV_WIN32_LOAD             18u

#define AUDIT_EV_COUNT                  19u

/* ---------- 审计级别 ---------- */
#define AUDIT_LVL_INFO     0u
#define AUDIT_LVL_WARN     1u
#define AUDIT_LVL_CRITICAL 2u

/* ---------- 环形缓冲尺寸 ---------- */
#define AUDIT_RING_SIZE  256u
#define AUDIT_PATH_MAX   64u

struct audit_entry {
    uint64_t seq;
    uint64_t tick;
    uint32_t type;
    uint32_t level;
    uint64_t caller_pid;
    uint64_t arg0;
    uint64_t arg1;
    uint32_t mode;
    uint32_t _pad;
    char     path[AUDIT_PATH_MAX];
};

void audit_init(void);

void audit_event(uint32_t type, uint32_t level,
                 uint64_t caller_pid,
                 uint64_t arg0, uint64_t arg1,
                 uint32_t mode, const char *path);

void audit_pid_violation(uint64_t caller_pid, uint64_t target_pid);
void audit_kernel_mem_violation(uint64_t caller_pid, uint64_t vaddr,
                                uint32_t mode);
void audit_critical_access(uint64_t caller_pid, const char *path,
                           uint32_t mode);
void audit_ita_failure(const char *path, uint64_t caller_pid);

uint64_t audit_total_count(void);
uint64_t audit_current_count(void);
int      audit_peek(uint64_t seq, struct audit_entry *out);
void     audit_dump_recent(int n);

#endif /* OMNIBRIDGE_AUDIT_H */
/*===OmniBridgeOs/kernel/arch/x64/audit.h 结束===*/