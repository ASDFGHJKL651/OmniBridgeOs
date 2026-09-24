#ifndef OMNIBRIDGE_PERMISSION_H
#define OMNIBRIDGE_PERMISSION_H

#include <stdint.h>
#include "task.h"

/*
 * 权限引擎 v0.1（第 9 步）
 *
 * 严格遵循 v5.0 技术规格书 §11 的执行顺序：
 *   1) 沙盒优先级检查（本步仅占位，拦截留步骤 16）
 *   2) 内核路径 / 内核地址"无条件拒绝"（唯一例外：
 *      level==9 && ui_token_valid，且只能通过专用调用 OB_ReadKernelFile /
 *      OB_ReadKernelMemory / OB_WriteKernelMemory / OB_InternalSign 进入）
 *   3) PID 0-99 保护（仅 PROCESS 资源类型）
 *   4) 权限 0/1 的隔离规则
 *   5) 常规 9 级权限比较
 *
 * 任何顺序调整都可能引入绕过窗口。
 *
 * 本步对以下内容做临时简化（步骤 11 / 15 / 16 会替换）：
 *   - 文件系统 VFS 未实现：权限 0 的文件操作返回 -ENOSYS；
 *     权限 1 仅按字符串前缀 /tmp/priv1_<pid>_ 判定。
 *   - 权限 0/1 的内存访问不做 mem_domain 边界检查（选项 A），
 *     非内核地址一律放行；真正的隔离由独立 PML4 在步骤 15 提供。
 *   - 权限 ≥ 2 的目录完整性等级比较只做等级数字粗判。
 *   - 沙盒拦截器 see_syscall_interceptor() 未实现，本步仅占位。
 */

/* ---------- 资源类型 ---------- */
#define OB_RES_FILE     0
#define OB_RES_MEMORY   1
#define OB_RES_PROCESS  2
#define OB_RES_IPC      3
#define OB_RES_CONFIG   4
#define OB_RES_COUNT    5

/* ---------- 访问模式 ---------- */
#define OB_ACCESS_READ   0x01
#define OB_ACCESS_WRITE  0x02
#define OB_ACCESS_EXEC   0x04
#define OB_ACCESS_DELETE 0x08
#define OB_ACCESS_ALL    0x0F

/* ---------- 判定结果（历史常量，保留供未来 SecMgr 查询接口使用） ---------- */
#define OB_PERM_DENIED   0
#define OB_PERM_ALLOWED  1

/* ---------- 沙盒标记（与 task.h 保持一致，避免重复定义） ---------- */
#ifndef OBSANDBOX_ACTIVE
#define OBSANDBOX_ACTIVE 0x01u
#endif

/* ---------- 主入口 ---------- */
/*
 * 返回 0 = 允许；返回负值 = 拒绝（通常是 OB_EPERM / OB_ENOSYS）。
 *
 * 参数：
 *   cur         调用者任务（必须非 NULL）
 *   res_type    OB_RES_*
 *   res_id      资源标识：
 *                 FILE    → 忽略，使用 path_hint
 *                 MEMORY  → 目标虚拟地址
 *                 PROCESS → 目标 PID
 *                 IPC     → IPC 句柄（本步未实现）
 *                 CONFIG  → 配置项 ID
 *   access_mode OB_ACCESS_*
 *   path_hint   文件路径 / 描述字符串；可为 NULL
 */
int check_permission(struct task_t *cur,
                     int res_type,
                     uint64_t res_id,
                     uint32_t access_mode,
                     const char *path_hint);

/* ---------- 内部子检查（供 permission.c 实现，也被 syscall.c 直接复用） ---------- */
int permission_check_process(struct task_t *cur, uint64_t target_pid,
                             uint32_t mode, const char *hint);
int permission_check_memory(struct task_t *cur, uint64_t vaddr,
                            uint32_t mode, const char *hint);
int permission_check_file(struct task_t *cur, const char *path,
                          uint32_t mode);
int permission_check_ipc(struct task_t *cur, uint64_t ipc_id,
                         uint32_t mode, const char *hint);
int permission_check_config(struct task_t *cur, uint64_t cfg_id,
                            uint32_t mode, const char *hint);

/* ---------- 路径辅助 ---------- */
/* 前缀匹配：成功返回 1，失败返回 0。 */
int path_starts_with(const char *path, const char *prefix);

/* /kernel、/kernel/、/system/kernel、/system/kernel/ 都视为命中 */
int path_is_kernel_protected(const char *path);

/* /system/critical、/system/critical/ 命中 */
int path_is_critical_dir(const char *path);

/* ---------- 内核虚拟地址区域判定 ---------- */
int vaddr_is_kernel(uint64_t vaddr);

/* ---------- 审计桩（步骤 10 会替换为环形缓冲实现） ----------
 * audit_pid_violation 由 task.c 提供（第 8 步已实现），此处只声明。
 */
void audit_kernel_mem_violation(uint64_t caller_pid, uint64_t vaddr, uint32_t mode);
void audit_critical_access(uint64_t caller_pid, const char *path, uint32_t mode);
extern void audit_pid_violation(uint64_t caller_pid, uint64_t target_pid);

#endif /* OMNIBRIDGE_PERMISSION_H */